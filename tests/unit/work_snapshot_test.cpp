#include "workspace_fixture.h"
// White-box module tests: snapshot round trips preserve durable ownership,
// stamps and selection order without persisting boot compatibility holds.

#include "authority/principal/owner_binding_records.h"
#include "durability/snapshot/snapshot_codec.h"
#include "foundation/crc32.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/work_replay.h"
#include "kernel/service/persistence/work_snapshot.h"
#include "work/task/task_records.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using ayni::durability::BatchCheck;
using ayni::durability::SnapshotMetadata;
using ayni::kernel::WorkModel;
using ayni::kernel::WorkSnapshotReader;
using ayni::kernel::WorkSnapshotWriter;
using ayni::work::GoalTable;
using ayni::work::TaskTable;

constexpr uint64_t instance_id = 0x1'0000'0001ULL;
constexpr uint64_t goal_id = 0x1'0000'0002ULL;
constexpr uint64_t task_id = 0x1'0000'0003ULL;

std::vector<uint64_t> store(std::size_t bytes) {
    return std::vector<uint64_t>((bytes + 7U) / 8U);
}

ayni::kernel::KernelCapacities capacities() {
    ayni::kernel::KernelCapacities result{};
    result.agent_capacity = 2;
    result.goal_type_capacity = 2;
    result.goal_capacity = 8;
    result.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
    result.goal_parameter_max_bytes = 32;
    result.session_capacity = 2;
    result.task_capacity = 4;
    result.strategy_capacity = 2;
    result.policy_intent_capacity = 2;
    result.task_step_capacity = 4;
    result.task_checkpoint_max_bytes = 16;
    result.command_capacity = 2;
    result.capability_capacity = 1;
    result.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    result.grant_capacity = 4;
    result.grant_delegation_depth_max = 4;
    return result;
}

class Model {
  public:
    explicit Model(ayni::kernel::KernelCapacities config = capacities(),
                   ayni::kernel::KernelLimits limits = {3, 4})
        : capacities_(config),
          agents_(store(ayni::work::AgentTable::storage_size(config.agent_capacity))),
          types_(store(ayni::catalog::GoalTypeRegistry::storage_size(config.goal_type_capacity))),
          goals_(store(
              GoalTable::storage_size(config.goal_capacity, config.goal_parameter_max_bytes))),
          calendars_(store(ayni::work::CalendarTable::storage_size(config.goal_capacity))),
          sessions_(store(ayni::authority::SessionTable::storage_size(config.session_capacity))),
          tasks_(store(TaskTable::storage_size(ayni::kernel::task_limits(config)))),
          strategies_(
              store(ayni::catalog::StrategyRegistry::storage_size(config.strategy_capacity))),
          commands_(store(
              ayni::kernel::CommandTable::storage_size(ayni::kernel::command_limits(config)))),
          capabilities_(store(ayni::catalog::CapabilityRegistry::storage_size(
              config.capability_capacity, config.authority_scope_max_bytes))),
          grants_(
              store(ayni::authority::GrantTable::storage_size(ayni::kernel::grant_limits(config)))),
          goal_authority_(store(ayni::work::GoalAuthorityTable::storage_size(
              ayni::kernel::goal_authority_limits(config)))),
          task_authority_(store(ayni::work::TaskAuthorityTable::storage_size(
              ayni::kernel::task_authority_limits(config)))),
          storage_(storage()), catalogs_(capacities_, storage_, ::ayni::test::codec_workspace()),
          model_(catalogs_, capacities_, limits, storage_, callbacks_,
                 ::ayni::test::codec_workspace()) {}

    WorkModel &get() { return model_; }

  private:
    // The stores WorkModel reads; the kernel's own stays unset.
    ayni::kernel::KernelStorage storage() {
        using ayni::kernel::Store;
        ayni::kernel::KernelStorage result;
        result.set(Store::agents, agents_.data());
        result.set(Store::goal_types, types_.data());
        result.set(Store::goals, goals_.data());
        result.set(Store::calendars, calendars_.data());
        result.set(Store::sessions, sessions_.data());
        result.set(Store::tasks, tasks_.data());
        result.set(Store::strategies, strategies_.data());
        result.set(Store::commands, commands_.data());
        result.set(Store::capabilities, capabilities_.data());
        result.set(Store::grants, grants_.data());
        result.set(Store::goal_authority, goal_authority_.data());
        result.set(Store::task_authority, task_authority_.data());
        return result;
    }

    ayni::kernel::KernelCapacities capacities_;
    std::vector<uint64_t> agents_, types_, goals_, calendars_, sessions_, tasks_, strategies_,
        commands_, capabilities_, grants_, goal_authority_, task_authority_;
    ayni::kernel::KernelStorage storage_;
    ayni::kernel::Catalogs catalogs_;
    ayni::kernel::CallbackGate callbacks_;
    WorkModel model_;
};

ayni::work::NewGoal goal(uint64_t parent = 0) {
    ayni::work::NewGoal result{};
    result.owner_instance_id = instance_id;
    result.parent_goal_id = parent;
    result.goal_type_version = 1;
    static_cast<void>(result.goal_type_id.assign({"timer", 5}));
    result.trigger.kind = AYNI_TRIGGER_ONCE;
    result.occurrence_wait_max_us = ayni::work::no_time;
    return result;
}

// ADR-0038: these fresh snapshot fixtures explicitly declare zero needs;
// production recovery never manufactures evidence for an old stored object.
void admit_fixture_work(WorkModel &model) {
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        const auto &goal = model.goals.at(index);
        if (model.goal_authority.contains(index, goal.goal_id)) {
            continue;
        }
        ayni::work::GoalAuthorityRecord record{};
        record.goal_id = goal.goal_id;
        record.stamp = goal.created;
        record.binding.established_seq = record.stamp.commit_seq;
        if (goal.parent_index != GoalTable::no_parent) {
            const auto &parent = model.goals.at(goal.parent_index);
            record.binding.parent = {
                parent.goal_id, model.goal_authority.at(goal.parent_index).binding.established_seq};
        }
        ASSERT_TRUE(
            ayni::work::valid_goal_authority_record(::ayni::test::codec_workspace(), record));
        ASSERT_EQ(model.goal_authority.install_validated(index, record), AYNI_OK);
    }
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        const auto &task = model.tasks.at(index);
        if (!model.task_authority.contains(task.slot, task.task_id)) {
            ASSERT_TRUE(model.inherit_task_metadata(
                task.task_id, {task.created_transition_id, task.created_transition_id}));
        }
    }
}

void populate(WorkModel &model) {
    ASSERT_EQ(model.agents.restore(100, instance_id, 1, AYNI_AGENT_RUNNING, {2, 2}),
              ayni::work::AgentTable::Restore::restored);
    uint32_t index = 0;
    ASSERT_EQ(model.goals.restore(goal(), goal_id, 0, {3, 3}, index), GoalTable::Restore::restored);
    ayni::work::GoalState state{};
    state.lifecycle = AYNI_GOAL_ACTIVE;
    state.revision = 1;
    state.taken = 1;
    state.last_task_id = task_id;
    ASSERT_EQ(model.goals.restore_state(index, state, {4, 4}), GoalTable::Restore::restored);
    const ayni::work::NewTask task{goal_id, instance_id, 123, index, 1, 1};
    ASSERT_EQ(model.tasks.restore(task, task_id, 0, {4, 4}), TaskTable::Restore::restored);
    ASSERT_TRUE(model.tasks.add_step(0, {"first", 5}, {5, 5}));
    model.tasks.progress(0, {5, 5});
    ASSERT_TRUE(model.tasks.add_step(0, {"second", 6}, {7, 7}));
    ayni::catalog::CatalogId writer;
    ASSERT_TRUE(writer.assign({"strategy", 8}));
    const uint8_t checkpoint[] = {9, 8, 7};
    model.tasks.set_checkpoint(0, writer, 1, {checkpoint, sizeof(checkpoint)}, {7, 7});
    model.tasks.progress(0, {7, 7});
    admit_fixture_work(model);
}

SnapshotMetadata metadata(WorkModel &model) {
    admit_fixture_work(model);
    SnapshotMetadata result{};
    result.generation = 4;
    result.covers_commit_seq = 100;
    result.max_transition_id = 100;
    result.boot_epoch = 1;
    result.writer_epoch = 2;
    const auto layout = ayni::kernel::measure_snapshot(model);
    result.payload_length = layout.payload_length;
    result.record_count = layout.record_count;
    return result;
}

std::vector<uint8_t> export_model(WorkModel &model, uint32_t page_size = 256) {
    admit_fixture_work(model);
    WorkSnapshotWriter writer(model);
    std::vector<uint8_t> page(page_size);
    std::vector<uint8_t> result;
    for (;;) {
        uint32_t size = 0;
        const auto next = writer.next(page.data(), page_size, size);
        EXPECT_NE(next, WorkSnapshotWriter::Page::no_room);
        if (next != WorkSnapshotWriter::Page::page) {
            break;
        }
        result.insert(result.end(), page.begin(), page.begin() + size);
    }
    return result;
}

BatchCheck import_model(WorkModel &model, const SnapshotMetadata &meta,
                        const std::vector<uint8_t> &bytes) {
    WorkSnapshotReader reader(model, meta);
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < ayni::durability::record_header_size) {
            return BatchCheck::corrupt;
        }
        const uint8_t *record = bytes.data() + at;
        const ayni::durability::RecordHeader header{
            ayni::foundation::load_le16(record), ayni::foundation::load_le16(record + 2),
            ayni::foundation::load_le32(record + 4), ayni::foundation::load_le32(record + 8)};
        at += ayni::durability::record_header_size;
        if (header.length > bytes.size() - at) {
            return BatchCheck::corrupt;
        }
        const auto check = reader.apply(header, bytes.data() + at);
        if (check != BatchCheck::ok) {
            return check;
        }
        at += header.length;
    }
    return reader.complete();
}

// ADR-0082: an optional journal-only extension is rejected in a snapshot.
TEST(WorkSnapshot, optional_goal_state_extensions_are_rejected) {
    Model original;
    populate(original.get());
    auto bytes = export_model(original.get());
    auto meta = metadata(original.get());
    const auto offset = bytes.size();
    bytes.resize(offset + ayni::durability::record_header_size);
    ayni::durability::write_record_header({ayni::durability::record_type::goal_state, 1, 0, 0},
                                          bytes.data() + offset);
    ++meta.record_count;
    meta.payload_length = bytes.size();
    Model restored;
    ASSERT_EQ(import_model(restored.get(), meta, bytes), BatchCheck::corrupt);
}

// ADR-0065: normal group preference snapshots retain the exact logical input frontier.
TEST(WorkSnapshot, group_preferences_preserve_logical_reopen_frontiers_across_pages) {
    Model original;
    ASSERT_TRUE(original.get().principals.restore_binding({7, 3}, {1, 1}));
    original.get().pause_groups.apply({1, 0, true}, instance_id, {2, 2}, {});
    original.get().pause_groups.apply({1, 1, false}, instance_id, {3, 3}, {1, 7});
    const auto bytes = export_model(original.get(), 128);
    EXPECT_EQ(bytes, export_model(original.get(), 4096));
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), bytes), BatchCheck::ok);
    const auto &row = restored.get().pause_groups.at(1);
    EXPECT_FALSE(row.paused);
    EXPECT_EQ(row.revision, 2U);
    EXPECT_EQ(row.input.incarnation, 1U);
    EXPECT_EQ(row.input.through, 7U);
}

// C-STORE-007, C-STORE-014: an arbitrarily paged snapshot keeps the exact
// durable view, including original transition stamps and checkpoint order.
TEST(WorkSnapshot, paged_round_trip_keeps_the_original_stamps_and_checkpoint) {
    Model original;
    populate(original.get());
    const auto bytes = export_model(original.get());
    const auto meta = metadata(original.get());
    ASSERT_GT(bytes.size(), 256U);
    EXPECT_EQ(bytes.size(), meta.payload_length);
    Model restored;
    ASSERT_EQ(import_model(restored.get(), meta, bytes), BatchCheck::ok);
    const auto &task = restored.get().tasks.at(0);
    EXPECT_EQ(task.created_transition_id, 4U);
    EXPECT_EQ(task.commit_seq, 7U);
    EXPECT_EQ(task.steps_seq, 7U);
    EXPECT_EQ(task.steps_before, 1U);
    EXPECT_EQ(task.checkpoint_seq, 7U);
    EXPECT_EQ(task.step_count, 2U);
    EXPECT_TRUE(restored.get().tasks.step(0, 0).equals({"first", 5}));
    EXPECT_TRUE(restored.get().tasks.step(0, 1).equals({"second", 6}));
    EXPECT_EQ(restored.get().tasks.checkpoint(0).size, 3U);
    EXPECT_EQ(std::memcmp(restored.get().tasks.checkpoint(0).data,
                          original.get().tasks.checkpoint(0).data, 3),
              0);
    EXPECT_EQ(restored.get().goals.at(0).created.transition_id, 3U);
    EXPECT_EQ(restored.get().goals.at(0).commit_seq, 4U);
    EXPECT_EQ(restored.get().agents.at(0).stamp.commit_seq, 2U);
}

// C-STORE-007 / C-LIMIT-007: the deepest legal Goal is part of the durable
// tree, even when the traversal has reached its final permitted depth.
TEST(WorkSnapshot, the_deepest_supported_goal_survives_a_paged_round_trip) {
    auto config = capacities();
    config.goal_capacity = AYNI_GOAL_DEPTH_MAX_MAX + 1U;
    const ayni::kernel::KernelLimits limits{AYNI_GOAL_DEPTH_MAX_MAX, 1};
    Model original(config, limits);
    ASSERT_EQ(original.get().agents.restore(100, instance_id, 1, AYNI_AGENT_RUNNING, {2, 2}),
              ayni::work::AgentTable::Restore::restored);
    uint64_t parent = 0;
    for (uint32_t depth = 0; depth <= AYNI_GOAL_DEPTH_MAX_MAX; ++depth) {
        uint32_t index = 0;
        const uint64_t id = goal_id + depth;
        ASSERT_EQ(
            original.get().goals.restore(goal(parent), id, 0, {3U + depth, 3U + depth}, index),
            GoalTable::Restore::restored);
        ASSERT_EQ(original.get().goals.at(index).depth, depth);
        parent = id;
    }
    Model restored(config, limits);
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    EXPECT_EQ(restored.get().goals.size(), config.goal_capacity);
    uint32_t last = 0;
    ASSERT_TRUE(restored.get().goals.find(parent, last));
    EXPECT_EQ(restored.get().goals.at(last).depth, AYNI_GOAL_DEPTH_MAX_MAX);
}

// C-STORE-007/014: an empty checkpoint still retains its writer, version
// and commit identity; an empty payload is not an absent checkpoint.
TEST(WorkSnapshot, an_empty_checkpoint_keeps_its_writer_and_commit_identity) {
    Model original;
    populate(original.get());
    ayni::catalog::CatalogId writer;
    ASSERT_TRUE(writer.assign({"strategy", 8}));
    original.get().tasks.set_checkpoint(0, writer, 2, {nullptr, 0}, {8, 8});
    original.get().tasks.progress(0, {8, 8});
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    const auto &task = restored.get().tasks.at(0);
    EXPECT_EQ(task.checkpoint_writer_version, 2U);
    EXPECT_TRUE(task.checkpoint_writer_id.equals({"strategy", 8}));
    EXPECT_EQ(task.checkpoint_seq, 8U);
    EXPECT_EQ(restored.get().tasks.checkpoint(0).size, 0U);
}

// C-STORE-007/011 / ADR-0035 decision 13: a too-small page cannot consume the
// selected command mapping; retrying with room must preserve all history.
TEST(WorkSnapshot, a_command_that_does_not_fit_is_written_on_the_next_page) {
    Model original;
    const ayni_object_ref_v1 object{0, goal_id, 1, AYNI_OBJECT_GOAL};
    original.get().commands.insert({7, 100, 1, 3, AYNI_INTENT_GOAL_CREATE, object});
    original.get().commands.insert({7, 200, 2, 4, AYNI_INTENT_GOAL_CREATE, object});
    const auto expected = export_model(original.get());
    WorkSnapshotWriter writer(original.get());
    uint8_t short_page[1]{};
    uint32_t size = 0;
    EXPECT_EQ(writer.next(short_page, sizeof(short_page), size), WorkSnapshotWriter::Page::no_room);
    EXPECT_EQ(size, 0U);
    std::vector<uint8_t> page(expected.size());
    ASSERT_EQ(writer.next(page.data(), static_cast<uint32_t>(page.size()), size),
              WorkSnapshotWriter::Page::page);
    page.resize(size);
    EXPECT_EQ(page, expected);
}

// ADR-0026 decision 7: a temporary inability to serve an object must not
// become a permanent hold when the log holding its original state rotates.
TEST(WorkSnapshot, compatibility_faults_and_holds_are_not_written) {
    Model original;
    populate(original.get());
    original.get().agents.fault_unregistered();
    original.get().tasks.hold_restored(0, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
    EXPECT_EQ(ayni::work::AgentTable::effective_lifecycle(original.get().agents.at(0)),
              AYNI_AGENT_FAULTED);
    EXPECT_EQ(TaskTable::effective_lifecycle(original.get().tasks.at(0)), AYNI_TASK_PAUSED);
    Model restored;
    ASSERT_EQ(restored.get().agents.add(100), ayni::work::AgentTable::AddResult::added);
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    EXPECT_EQ(ayni::work::AgentTable::effective_lifecycle(restored.get().agents.at(0)),
              AYNI_AGENT_RUNNING);
    EXPECT_EQ(TaskTable::effective_lifecycle(restored.get().tasks.at(0)), AYNI_TASK_READY);
    EXPECT_EQ(TaskTable::effective_hold_status(restored.get().tasks.at(0)), AYNI_OK);
}

// Static snapshots contain parents that have already ended. Journal GOAL
// creation rules cannot import such a retained tree.
TEST(WorkSnapshot, terminal_parent_and_child_keep_their_reclamation_order) {
    Model original;
    populate(original.get());
    uint32_t child = 0;
    ASSERT_EQ(original.get().goals.restore(goal(goal_id), goal_id + 5U, 0, {9, 9}, child),
              GoalTable::Restore::restored);
    original.get().tasks.apply(0, ayni::work::TaskChange::cancel, {10, 10});
    original.get().goals.apply(0, ayni::work::GoalChange::cancel, {10, 10});
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    ASSERT_EQ(restored.get().goals.size(), 2U);
    EXPECT_EQ(restored.get().goals.at(0).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(restored.get().goals.at(1).parent_index, 0U);
    EXPECT_EQ(restored.get().goals.at(1).depth, 1U);
    EXPECT_EQ(restored.get().goals.at(1).commit_seq, 10U);
}

// An absent owner still owns its bounded command retry window.
TEST(WorkSnapshot, commands_keep_their_floor_and_original_acceptance_order) {
    Model original;
    auto &commands = original.get().commands;
    const ayni_object_ref_v1 object{0, goal_id, 1, AYNI_OBJECT_GOAL};
    commands.insert({7, 100, 1, 3, AYNI_INTENT_GOAL_CREATE, object});
    commands.insert({7, 200, 2, 4, AYNI_INTENT_GOAL_CREATE, object});
    commands.insert({7, 1, 3, 5, AYNI_INTENT_GOAL_CREATE, object});
    EXPECT_EQ(commands.floor(7), 100U);
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    const ayni::kernel::CommandMapping *mapping = nullptr;
    EXPECT_EQ(restored.get().commands.find(7, 1, 3, mapping),
              ayni::kernel::CommandTable::Lookup::retried);
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->commit_seq, 5U);
    EXPECT_EQ(restored.get().commands.floor(7), 100U);
    auto smaller_config = capacities();
    smaller_config.command_capacity = 1;
    Model smaller(smaller_config);
    ASSERT_EQ(import_model(smaller.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    EXPECT_EQ(smaller.get().commands.find(7, 1, 3, mapping),
              ayni::kernel::CommandTable::Lookup::retried);
    EXPECT_EQ(smaller.get().commands.floor(7), 200U);
    EXPECT_EQ(
        smaller.get().commands.restore_checked({8, 2, 1, 6, AYNI_INTENT_GOAL_CREATE, object}, 0),
        ayni::kernel::CommandTable::Restore::no_room);
}

TEST(WorkSnapshot, unknown_required_records_and_incomplete_tasks_refuse_recovery) {
    Model model;
    SnapshotMetadata meta{};
    meta.record_count = 1;
    WorkSnapshotReader reader(model.get(), meta);
    EXPECT_EQ(reader.apply({999, 1, ayni::durability::record_required, 0}, nullptr),
              BatchCheck::unsupported);
    reader.reset(meta);
    EXPECT_EQ(reader.apply({999, 1, 0, 0}, nullptr), BatchCheck::unsupported);
    // ADR-0038: an optional unknown record cannot replace the required work
    // cleanup fact, including an explicit idle scan in an otherwise empty snapshot.
    EXPECT_EQ(reader.complete(), BatchCheck::corrupt);
    Model original;
    populate(original.get());
    auto bytes = export_model(original.get());
    bytes.resize(bytes.size() -
                 (ayni::durability::record_header_size + ayni::work::task_state_record_size));
    auto incomplete = metadata(original.get());
    --incomplete.record_count;
    EXPECT_EQ(import_model(model.get(), incomplete, bytes), BatchCheck::corrupt);
}

TEST(WorkSnapshot, writer_reports_a_record_that_does_not_fit_and_reset_is_repeatable) {
    Model original;
    populate(original.get());
    WorkSnapshotWriter writer(original.get());
    uint8_t small[1]{};
    uint32_t size = 123;
    EXPECT_EQ(writer.next(small, sizeof(small), size), WorkSnapshotWriter::Page::no_room);
    EXPECT_EQ(size, 0U);
    writer.reset();
    std::vector<uint8_t> bytes(ayni::kernel::snapshot_record_max(capacities()));
    EXPECT_EQ(writer.next(bytes.data(), static_cast<uint32_t>(bytes.size()), size),
              WorkSnapshotWriter::Page::page);
    EXPECT_EQ(size, metadata(original.get()).payload_length);
    EXPECT_EQ(ayni::kernel::snapshot_record_max(capacities()) <= bytes.size(), true);
}

TEST(WorkSnapshot, reload_discard_clears_sessions_and_due_work_but_keeps_registration) {
    Model model;
    ASSERT_EQ(model.get().agents.add(100), ayni::work::AgentTable::AddResult::added);
    populate(model.get());
    ASSERT_TRUE(model.get().sessions.open({7, 8, 0, 0}));
    ayni::work::DueWork work{};
    uint64_t next_us = 0;
    static_cast<void>(model.get().next_due_work(0, UINT64_MAX, work, next_us));
    ayni::kernel::WorkReplay(model.get()).discard_for_reload();
    EXPECT_EQ(model.get().agents.size(), 1U);
    EXPECT_EQ(model.get().agents.at(0).lifecycle, AYNI_AGENT_REGISTERED);
    EXPECT_EQ(model.get().goals.size(), 0U);
    EXPECT_EQ(model.get().tasks.size(), 0U);
    EXPECT_EQ(model.get().sessions.find(7), nullptr);
    EXPECT_FALSE(model.get().next_due_work(0, UINT64_MAX, work, next_us));
    EXPECT_EQ(next_us, ayni::work::no_time);
    EXPECT_FALSE(model.get().ids.available());
}

// C-STORE-007: READY fairness and terminal-object reclamation depend on
// their original commits, not the snapshot's later covers_commit_seq.
TEST(WorkSnapshot, ready_and_reclamation_choices_match_the_uncompacted_model) {
    Model original;
    populate(original.get());
    uint32_t second_goal = 0;
    ASSERT_EQ(original.get().goals.restore(goal(), goal_id + 1U, 0, {8, 8}, second_goal),
              GoalTable::Restore::restored);
    ASSERT_EQ(original.get().tasks.restore({goal_id + 1U, instance_id, 456, second_goal, 1, 1},
                                           task_id + 1U, 0, {9, 9}),
              TaskTable::Restore::restored);
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    uint32_t first = 0;
    uint32_t second = 0;
    ASSERT_TRUE(original.get().tasks.next_ready(101, first));
    ASSERT_TRUE(restored.get().tasks.next_ready(101, second));
    EXPECT_EQ(original.get().tasks.at(first).task_id, task_id);
    EXPECT_EQ(restored.get().tasks.at(second).task_id, task_id);

    // Fill the task tables, ending the lower-id task later than its peer.
    for (WorkModel *model : {&original.get(), &restored.get()}) {
        model->tasks.apply(0, ayni::work::TaskChange::cancel, {20, 20});
        model->tasks.apply(1, ayni::work::TaskChange::cancel, {15, 15});
        for (uint32_t index = 2; index < 4; ++index) {
            ASSERT_TRUE(model->tasks.insert({goal_id, instance_id, 0, 0, index, 1}, task_id + index,
                                            {30U + index, 30U + index}));
            model->tasks.apply(index, ayni::work::TaskChange::cancel, {40U + index, 40U + index});
        }
    }
    Model after_terminal_snapshot;
    ASSERT_EQ(import_model(after_terminal_snapshot.get(), metadata(original.get()),
                           export_model(original.get())),
              BatchCheck::ok);
    for (WorkModel *model : {&original.get(), &after_terminal_snapshot.get()}) {
        ASSERT_TRUE(
            model->tasks.insert({goal_id, instance_id, 0, 0, 8, 1}, task_id + 99U, {90, 90}));
        uint32_t created = 0;
        ASSERT_TRUE(model->tasks.find(task_id + 99U, created));
        EXPECT_EQ(model->tasks.at(created).replaced_task_id, task_id + 1U);
    }
}

TEST(WorkSnapshot, a_retained_object_can_name_a_predecessor_already_reclaimed) {
    Model original;
    populate(original.get());
    original.get().tasks.apply(0, ayni::work::TaskChange::cancel, {10, 10});
    original.get().goals.apply(0, ayni::work::GoalChange::cancel, {10, 10});
    uint32_t index = 0;
    ASSERT_EQ(original.get().goals.restore(goal(), goal_id + 50U, goal_id, {11, 11}, index),
              GoalTable::Restore::restored);
    original.get().tasks.remove_goal(index);
    Model restored;
    ASSERT_EQ(import_model(restored.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::ok);
    EXPECT_EQ(restored.get().goals.size(), 1U);
    EXPECT_EQ(restored.get().goals.at(0).replaced_goal_id, goal_id);
    EXPECT_EQ(restored.get().goals.at(0).created.transition_id, 11U);
}

// C-LIMIT-007: valid state written under larger limits is capacity failure,
// not corruption and not permission to omit checkpoints or steps.
TEST(WorkSnapshot, smaller_object_and_checkpoint_capacities_report_no_room) {
    Model original;
    populate(original.get());
    const auto bytes = export_model(original.get());
    const auto meta = metadata(original.get());
    for (const bool steps : {false, true}) {
        auto config = capacities();
        if (steps) {
            config.task_step_capacity = 1;
        } else {
            config.task_checkpoint_max_bytes = 2;
        }
        Model smaller(config);
        EXPECT_EQ(import_model(smaller.get(), meta, bytes), BatchCheck::no_room);
        ayni::kernel::WorkReplay(smaller.get()).discard();
        EXPECT_EQ(smaller.get().tasks.size(), 0U);
        EXPECT_EQ(smaller.get().goals.size(), 0U);
    }
    ASSERT_EQ(original.get().agents.restore(200, instance_id + 100U, 1, AYNI_AGENT_RUNNING, {9, 9}),
              ayni::work::AgentTable::Restore::restored);
    auto fewer_agents = capacities();
    fewer_agents.agent_capacity = 1;
    Model smaller(fewer_agents);
    EXPECT_EQ(import_model(smaller.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::no_room);
}

TEST(WorkSnapshot, smaller_goal_parameter_limit_is_capacity_failure) {
    Model original;
    ASSERT_EQ(original.get().agents.restore(100, instance_id, 1, AYNI_AGENT_RUNNING, {2, 2}),
              ayni::work::AgentTable::Restore::restored);
    auto parameterized = goal();
    const uint8_t parameters[] = {1, 2, 3};
    parameterized.parameters = {parameters, sizeof(parameters)};
    uint32_t index = 0;
    ASSERT_EQ(original.get().goals.restore(parameterized, goal_id, 0, {3, 3}, index),
              GoalTable::Restore::restored);
    auto config = capacities();
    config.goal_parameter_max_bytes = 2;
    Model smaller(config);
    EXPECT_EQ(import_model(smaller.get(), metadata(original.get()), export_model(original.get())),
              BatchCheck::no_room);
}

TEST(WorkSnapshot, object_stamp_beyond_the_snapshot_boundary_is_corrupt) {
    Model original;
    populate(original.get());
    auto bytes = export_model(original.get());
    // The first record is an instance: its snapshot stamp follows the
    // 24-byte INSTANCE body. Mutate the original commit beyond covers.
    ayni::foundation::store_le64(bytes.data() + ayni::durability::record_header_size + 32U, 101);
    Model restored;
    EXPECT_EQ(import_model(restored.get(), metadata(original.get()), bytes), BatchCheck::corrupt);
}

TEST(WorkSnapshot, bytes_are_independent_of_this_boots_monotonic_clock) {
    Model original;
    populate(original.get());
    auto scheduled = goal();
    scheduled.trigger.kind = AYNI_TRIGGER_INTERVAL;
    scheduled.trigger.delay_us = 500;
    scheduled.trigger.period_us = 1000000;
    scheduled.created_us = 1000;
    scheduled.expiry_us = 10000;
    uint32_t index = 0;
    ASSERT_EQ(original.get().goals.restore(scheduled, goal_id + 99U, 0, {9, 9}, index),
              GoalTable::Restore::restored);
    const auto before = export_model(original.get());
    original.get().goals.reanchor(99999999);
    original.get().tasks.reanchor(99999999);
    EXPECT_EQ(export_model(original.get()), before);
}

// C-STORE-006: header and marker corruption never masquerade as a commit.
TEST(SnapshotCodec, every_header_and_marker_byte_is_checked) {
    SnapshotMetadata meta{};
    meta.generation = 2;
    meta.covers_commit_seq = 10;
    meta.max_transition_id = 10;
    meta.boot_epoch = 1;
    meta.writer_epoch = 1;
    std::array<uint8_t, ayni::durability::snapshot_header_size> header{};
    ayni::durability::write_snapshot_header(meta, header.data());
    SnapshotMetadata read{};
    ASSERT_EQ(ayni::durability::read_snapshot_header(header.data(), header.size(), read),
              BatchCheck::ok);
    for (std::size_t index = 0; index < header.size(); ++index) {
        auto damaged = header;
        damaged[index] ^= 1U;
        EXPECT_EQ(ayni::durability::read_snapshot_header(damaged.data(), damaged.size(), read),
                  BatchCheck::corrupt)
            << index;
    }
    std::array<uint8_t, ayni::durability::snapshot_marker_size> marker{};
    ayni::durability::write_snapshot_marker(meta, marker.data());
    for (std::size_t index = 0; index < marker.size(); ++index) {
        auto damaged = marker;
        damaged[index] ^= 1U;
        EXPECT_EQ(ayni::durability::read_snapshot_marker(damaged.data(), damaged.size(), meta),
                  BatchCheck::corrupt)
            << index;
        EXPECT_EQ(ayni::durability::read_snapshot_marker(marker.data(),
                                                         static_cast<uint32_t>(index), meta),
                  BatchCheck::corrupt)
            << index;
    }
}

TEST(SnapshotCodec, newer_schema_is_unsupported_and_crc_continues_between_pages) {
    SnapshotMetadata meta{};
    meta.generation = 1;
    meta.writer_epoch = 1;
    std::array<uint8_t, ayni::durability::snapshot_header_size> header{};
    ayni::durability::write_snapshot_header(meta, header.data());
    constexpr uint32_t schema_at = 4;
    constexpr uint32_t crc_at = ayni::durability::snapshot_header_size - sizeof(uint32_t);
    ayni::foundation::store_le16(header.data() + schema_at, ayni::durability::snapshot_schema + 1U);
    ayni::foundation::store_le32(header.data() + crc_at,
                                 ayni::foundation::crc32(header.data(), crc_at));
    SnapshotMetadata read{};
    EXPECT_EQ(ayni::durability::read_snapshot_header(header.data(), header.size(), read),
              BatchCheck::unsupported);
    const uint8_t bytes[] = {1, 2, 3, 4, 5, 6};
    const uint32_t prefix = ayni::durability::snapshot_crc32(bytes, 2);
    EXPECT_EQ(ayni::durability::snapshot_crc32(bytes + 2, 4, prefix),
              ayni::foundation::crc32(bytes, sizeof(bytes)));
}

} // namespace

// C-STORE-003/006: CRC-correct metadata cannot wrap the logical baseline.
TEST(SnapshotCodec, coverage_cannot_exceed_the_transition_baseline) {
    for (const uint64_t covers : {uint64_t{1}, UINT64_MAX}) {
        ayni::durability::SnapshotMetadata meta{};
        meta.generation = 1;
        meta.writer_epoch = 1;
        meta.covers_commit_seq = covers;
        std::array<uint8_t, ayni::durability::snapshot_header_size> header{};
        ayni::durability::write_snapshot_header(meta, header.data());
        ayni::durability::SnapshotMetadata read{};
        EXPECT_EQ(ayni::durability::read_snapshot_header(header.data(), header.size(), read),
                  ayni::durability::BatchCheck::corrupt);
    }
}

// records.md: a slot's boot_epoch fits the 32-bit epoch of an ID (C-ID-009).
TEST(SnapshotCodec, boot_epoch_beyond_the_id_epoch_is_corrupt) {
    const uint64_t largest = ayni::foundation::max_boot_epoch;
    for (const uint64_t boot_epoch : {largest, largest + 1U}) {
        ayni::durability::SnapshotMetadata meta{};
        meta.generation = 1;
        meta.writer_epoch = 1;
        meta.covers_commit_seq = 1;
        meta.max_transition_id = 1;
        meta.boot_epoch = boot_epoch;
        std::array<uint8_t, ayni::durability::snapshot_header_size> header{};
        ayni::durability::write_snapshot_header(meta, header.data());
        ayni::durability::SnapshotMetadata read{};
        EXPECT_EQ(ayni::durability::read_snapshot_header(header.data(), header.size(), read),
                  boot_epoch == largest ? ayni::durability::BatchCheck::ok
                                        : ayni::durability::BatchCheck::corrupt)
            << boot_epoch;
    }
}

// records.md: SNAPSHOT_COMMIT names one of the two slots and a generation,
// and covers no more than its transition baseline, which stays below 2^63.
TEST(SnapshotCodec, an_activation_reads_back_only_what_a_writer_can_store) {
    using ayni::durability::SnapshotActivation;
    const uint64_t largest = ayni::durability::transition_id_max;
    std::array<uint8_t, ayni::durability::snapshot_commit_record_length> body{};
    for (const SnapshotActivation &valid : {SnapshotActivation{1, 0x1234, 2, 10, 12},
                                            SnapshotActivation{0, 0, 1, largest, largest}}) {
        ayni::durability::write_snapshot_activation(valid, body.data());
        SnapshotActivation read{};
        ASSERT_TRUE(ayni::durability::read_snapshot_activation(body.data(), body.size(), read));
        EXPECT_EQ(read.slot, valid.slot);
        EXPECT_EQ(read.payload_crc, valid.payload_crc);
        EXPECT_EQ(read.generation, valid.generation);
        EXPECT_EQ(read.covers_commit_seq, valid.covers_commit_seq);
        EXPECT_EQ(read.max_transition_id, valid.max_transition_id);
        EXPECT_FALSE(
            ayni::durability::read_snapshot_activation(body.data(), body.size() - 1U, read));
    }
    const SnapshotActivation invalid[] = {
        {2, 0, 2, 10, 12},           // a third slot
        {1, 0, 0, 10, 12},           // generation 0
        {1, 0, 2, 10, largest + 1U}, // a transition_id beyond 2^63 - 1
        {1, 0, 2, 13, 12},           // covers past its baseline
    };
    for (const SnapshotActivation &activation : invalid) {
        ayni::durability::write_snapshot_activation(activation, body.data());
        SnapshotActivation read{};
        EXPECT_FALSE(ayni::durability::read_snapshot_activation(body.data(), body.size(), read))
            << activation.slot << " " << activation.generation << " "
            << activation.covers_commit_seq << " " << activation.max_transition_id;
    }
}
