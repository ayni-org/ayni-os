// MA-006 A/F through the C ABI: the first binding shares startup, sessions
// require its durable fact, and schema 2 recovery requires that saved owner.
// Compatible authorized work resumes without losing independent user pauses.

#include "authority/principal/owner_binding_records.h"
#include "durability/batch/batch_codec.h"
#include "kernel/command/command_records.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/host_authority.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace ayni::test;
using ayni::authority::OwnerBindingRecord;
using ayni::hosts::sim::CutAppend;
using ayni::hosts::sim::StoredBatch;

struct StoredBinding {
    OwnerBindingRecord owner;
    uint64_t transition_id;
    uint64_t commit_seq;
};

std::vector<StoredBinding> stored_bindings(Node &node) {
    std::vector<StoredBinding> found;
    for (const StoredBatch &stored : node.sim().log()) {
        if (stored.torn) {
            continue;
        }
        ayni::durability::BatchView batch{};
        EXPECT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                  ayni::durability::BatchCheck::ok);
        ayni::durability::RecordReader reader(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        for (;;) {
            const auto next = reader.next(header, body);
            if (next == ayni::durability::RecordReader::Next::end) {
                break;
            }
            EXPECT_EQ(next, ayni::durability::RecordReader::Next::record);
            if (next != ayni::durability::RecordReader::Next::record) {
                break;
            }
            if (header.type != ayni::authority::owner_binding_record_type) {
                continue;
            }
            EXPECT_EQ(header.version, ayni::authority::owner_binding_record_version);
            EXPECT_EQ(header.flags, ayni::durability::record_required);
            OwnerBindingRecord owner{};
            EXPECT_TRUE(ayni::authority::read_owner_binding_record(body, header.length, owner));
            found.push_back({owner, batch.identity.transition_id, batch.identity.commit_seq});
        }
    }
    return found;
}

void advance_deadline(Node &node, const ayni_step_report_v1 &report) {
    if (report.more_ready == 0U && node.sim().pending_storage() == 0U &&
        report.next_deadline_us != UINT64_MAX && report.next_deadline_us > node.sim().now_us()) {
        node.sim().advance_us(report.next_deadline_us - node.sim().now_us());
    }
}

// Stop before completing the startup append: the binding and instances are
// queryable in memory, while their persistent acceptance is still pending.
void enter_running_before_startup_is_stored(Node &node) {
    node.create_and_start();
    for (unsigned round = 0; round < 1000; ++round) {
        const auto report = node.step(1);
        if (node.lifecycle() != AYNI_LIFECYCLE_RECOVERING) {
            ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
            return;
        }
        node.sim().complete_storage();
        advance_deadline(node, report);
    }
    FAIL() << "startup did not enter RUNNING";
}

void store_through(Node &node, uint64_t commit_seq) {
    for (unsigned round = 0; round < 1000 && node.info().durable_seq < commit_seq; ++round) {
        const auto report = node.step(1);
        node.sim().complete_storage();
        advance_deadline(node, report);
    }
    ASSERT_GE(node.info().durable_seq, commit_seq);
}

void register_folder_node(ayni_kernel_t *kernel, const ayni_principal_desc_v1 &owner) {
    ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
    for (const uint64_t agent_id : {agent_a, agent_b}) {
        const auto agent = agent_desc(agent_id);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
    }
    const auto folder =
        goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE));
    ASSERT_EQ(ayni_register_goal_type(kernel, &folder), AYNI_OK);
}

void expect_same_log(Node &node, const std::vector<StoredBatch> &before) {
    ASSERT_EQ(node.sim().log().size(), before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(node.sim().log()[index].commit_seq, before[index].commit_seq);
        EXPECT_EQ(node.sim().log()[index].bytes, before[index].bytes);
        EXPECT_EQ(node.sim().log()[index].torn, before[index].torn);
    }
}

// A complete log from before OWNER_BINDING existed. Preserve every batch's
// identity and schema, replacing its checksum and commit marker after removing
// that record. An optional command principal change isolates retry-window
// evidence from the USER Goal's still matching origin.
std::vector<StoredBatch> without_binding(Node &source, uint64_t command_principal = 0) {
    auto log = source.sim().log();
    for (auto &stored : log) {
        ayni::durability::BatchView batch{};
        EXPECT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                  ayni::durability::BatchCheck::ok);
        std::vector<uint8_t> bytes(stored.bytes.size());
        ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity,
                                             batch.schema);
        ayni::durability::RecordReader reader(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        for (;;) {
            const auto next = reader.next(header, body);
            if (next == ayni::durability::RecordReader::Next::end) {
                break;
            }
            EXPECT_EQ(next, ayni::durability::RecordReader::Next::record);
            if (next != ayni::durability::RecordReader::Next::record) {
                break;
            }
            if (header.type == ayni::authority::owner_binding_record_type) {
                continue;
            }
            if (header.type == ayni::kernel::command_record_type && command_principal != 0U) {
                ayni::kernel::CommandMapping mapping{};
                uint64_t floor = 0;
                EXPECT_TRUE(ayni::kernel::read_command_record(body, header.length, mapping, floor));
                mapping.principal_id = command_principal;
                std::vector<uint8_t> command_body(header.length);
                ayni::kernel::write_command_record(mapping, floor, command_body.data());
                EXPECT_TRUE(writer.add_record(header, command_body.data()));
            } else {
                EXPECT_TRUE(writer.add_record(header, body));
            }
        }
        const auto size = writer.finish();
        EXPECT_NE(size, 0U);
        bytes.resize(size);
        stored.bytes = std::move(bytes);
    }
    return log;
}

TEST(OwnerBindingApi,
     first_binding_and_instances_share_a_transition_and_sessions_wait_for_durability) {
    WorkNode work;
    enter_running_before_startup_is_stored(work.node());
    const auto a = work.agent(agent_a);
    const auto b = work.agent(agent_b);
    ASSERT_GT(a.commit_seq, work.node().info().durable_seq);
    EXPECT_EQ(b.commit_seq, a.commit_seq);
    EXPECT_TRUE(stored_bindings(work.node()).empty());

    const auto session = session_desc();
    uint64_t session_id = 91;
    EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_NOT_READY);
    EXPECT_EQ(session_id, 91U);
    store_through(work.node(), a.commit_seq);

    const auto bindings = stored_bindings(work.node());
    ASSERT_EQ(bindings.size(), 1U);
    EXPECT_EQ(bindings[0].commit_seq, a.commit_seq);
    EXPECT_NE(bindings[0].transition_id, 0U);
    EXPECT_EQ(bindings[0].owner.principal_id, owner_id);
    EXPECT_EQ(bindings[0].owner.binding_revision, owner_binding);
    EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_OK);
    EXPECT_NE(session_id, 0U);
    EXPECT_NE(session_id, 91U);
}

TEST(OwnerBindingApi, principal_without_any_agent_still_has_a_durable_startup_binding) {
    Node node;
    node.on_create([](ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
    });
    node.run();
    const auto bindings = stored_bindings(node);
    ASSERT_EQ(bindings.size(), 1U);
    EXPECT_EQ(bindings[0].owner.principal_id, owner_id);
    EXPECT_EQ(bindings[0].owner.binding_revision, owner_binding);
    EXPECT_LE(bindings[0].commit_seq, node.info().durable_seq);
    const auto session = session_desc();
    uint64_t session_id = 0;
    EXPECT_EQ(ayni_open_session(node.kernel(), &session, &session_id), AYNI_OK);
    EXPECT_NE(session_id, 0U);
}

TEST(OwnerBindingApi, bad_session_binding_revision_does_not_write_the_output_or_consume_an_id) {
    WorkNode work;
    work.run();
    auto session = session_desc();
    uint64_t session_id = 91;
    session.binding_revision = owner_binding + 1U;
    EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_DENIED);
    EXPECT_EQ(session_id, 91U);
    session.binding_revision = 0;
    EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(session_id, 91U);
    session.binding_revision = owner_binding;
    session.reserved = 1;
    EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(session_id, 91U);
    session.reserved = 0;
    ASSERT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_OK);
    EXPECT_EQ(session_id, work.session() + 1U);
}

TEST(OwnerBindingApi, restarting_with_the_same_binding_does_not_write_another_binding_record) {
    WorkNode work;
    work.run();
    const auto before = stored_bindings(work.node());
    ASSERT_EQ(before.size(), 1U);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const auto after = stored_bindings(work.node());
    ASSERT_EQ(after.size(), 1U);
    EXPECT_EQ(after[0].transition_id, before[0].transition_id);
    EXPECT_EQ(after[0].commit_seq, before[0].commit_seq);
    work.reopen_session();
    EXPECT_NE(work.session(), 0U);
}

TEST(OwnerBindingApi,
     changed_principal_or_binding_revision_restricts_without_writing_or_losing_goals) {
    for (const bool change_principal : {false, true}) {
        SCOPED_TRACE(change_principal);
        WorkNode work;
        work.run();
        const auto body = WorkNode::create_body(work.instance(agent_a), 0, "folder");
        const auto created = work.create(body);
        ASSERT_EQ(created.status, AYNI_OK);
        store_through(work.node(), created.result.commit_seq);
        const auto goal_before = work.goal(created.result.object.id);
        const auto log_before = work.node().sim().log();
        auto expected = owner_desc();
        if (change_principal) {
            ++expected.principal_id;
        } else {
            ++expected.binding_revision;
        }
        work.node().on_create(
            [expected](ayni_kernel_t *kernel) { register_folder_node(kernel, expected); });
        work.node().reboot();
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
        expect_same_log(work.node(), log_before);
        const auto goal_after = work.goal(created.result.object.id);
        EXPECT_EQ(goal_after.goal_id, goal_before.goal_id);
        EXPECT_EQ(goal_after.owner_instance_id, goal_before.owner_instance_id);
        EXPECT_EQ(goal_after.origin_principal_id, goal_before.origin_principal_id);
        EXPECT_EQ(goal_after.origin_binding_revision, goal_before.origin_binding_revision);
        EXPECT_EQ(goal_after.revision, goal_before.revision);
        auto session = session_desc(AYNI_CHANNEL_MANAGEMENT, expected.principal_id);
        session.binding_revision = expected.binding_revision;
        uint64_t session_id = 91;
        EXPECT_EQ(ayni_open_session(work.kernel(), &session, &session_id), AYNI_NOT_READY);
        EXPECT_EQ(session_id, 91U);
    }
}

TEST(OwnerBindingApi, legacy_user_goal_evidence_rejects_a_different_or_missing_host_owner) {
    WorkNode source;
    source.run();
    const auto created =
        source.create(WorkNode::create_body(source.instance(agent_a), 0, "folder"));
    ASSERT_EQ(created.status, AYNI_OK);
    store_through(source.node(), created.result.commit_seq);
    const auto legacy = without_binding(source.node());
    for (unsigned mismatch = 0; mismatch < 3; ++mismatch) {
        SCOPED_TRACE(mismatch);
        Node later;
        later.sim().log() = legacy;
        ASSERT_TRUE(stored_bindings(later).empty());
        later.on_create([mismatch](ayni_kernel_t *kernel) {
            if (mismatch != 2U) {
                auto owner = owner_desc();
                if (mismatch == 0U) {
                    ++owner.principal_id;
                } else {
                    ++owner.binding_revision;
                }
                register_folder_node(kernel, owner);
                return;
            }
            for (const uint64_t agent_id : {agent_a, agent_b}) {
                const auto agent = agent_desc(agent_id);
                ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            }
            const auto folder =
                goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE));
            ASSERT_EQ(ayni_register_goal_type(kernel, &folder), AYNI_OK);
        });
        later.create_and_start();
        later.recover();
        ASSERT_EQ(later.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(later.info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
        expect_same_log(later, legacy);
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        ASSERT_EQ(ayni_get_goal(later.kernel(), created.result.object.id, &goal), AYNI_OK);
        EXPECT_EQ(goal.origin_principal_id, owner_id);
        EXPECT_EQ(goal.origin_binding_revision, owner_binding);
        EXPECT_EQ(goal.revision, created.result.object.revision);
    }
}

TEST(OwnerBindingApi, legacy_command_principal_conflict_rejects_even_when_goal_origin_matches) {
    WorkNode source;
    source.run();
    const auto body = WorkNode::create_body(source.instance(agent_a), 0, "folder");
    auto intent = source.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.command_id = 17;
    const auto created = source.submit(intent);
    ASSERT_EQ(created.status, AYNI_OK);
    store_through(source.node(), created.result.commit_seq);
    const auto legacy = without_binding(source.node(), owner_id + 1U);
    WorkNode later;
    later.node().sim().log() = legacy;
    ASSERT_TRUE(stored_bindings(later.node()).empty());
    later.node().create_and_start();
    later.node().recover();
    ASSERT_EQ(later.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(later.node().info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
    expect_same_log(later.node(), legacy);
    EXPECT_EQ(later.goal(created.result.object.id).origin_principal_id, owner_id);
    EXPECT_EQ(later.goal(created.result.object.id).origin_binding_revision, owner_binding);
}

// ADR-0038: development format break forbids owner adoption for existing work.
TEST(OwnerBindingApi, missing_binding_refuses_even_a_matching_host_without_new_writes) {
    WorkNode source;
    source.run();
    const auto created =
        source.create(WorkNode::create_body(source.instance(agent_a), 0, "folder"));
    ASSERT_EQ(created.status, AYNI_OK);
    store_through(source.node(), created.result.commit_seq);
    const auto unbound = without_binding(source.node());
    WorkNode later;
    later.node().sim().log() = unbound;
    later.node().create_and_start();
    later.node().recover();
    ASSERT_EQ(later.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(later.node().info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
    EXPECT_TRUE(stored_bindings(later.node()).empty());
    expect_same_log(later.node(), unbound);
    EXPECT_TRUE(later.strategy().seen.empty());
}

TEST(OwnerBindingApi, legacy_command_without_any_goal_still_requires_its_host_owner) {
    WorkNode source;
    source.run();
    const auto instance = source.agent(agent_a);
    ayni_instance_target_v1 target{};
    target.struct_size = sizeof(target);
    target.instance_id = instance.instance_id;
    auto intent = source.intent(AYNI_INTENT_INSTANCE_PAUSE, &target, instance.revision);
    intent.command_id = 17;
    const auto paused = source.submit(intent);
    ASSERT_EQ(paused.status, AYNI_OK);
    store_through(source.node(), paused.result.commit_seq);
    const auto legacy = without_binding(source.node());
    for (const bool matching_host : {false, true}) {
        SCOPED_TRACE(matching_host);
        Node later;
        if (matching_host) {
            later.on_create(
                [](ayni_kernel_t *kernel) { register_folder_node(kernel, owner_desc()); });
        }
        later.sim().log() = legacy;
        later.create_and_start();
        later.recover();
        ASSERT_EQ(later.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(later.info().restriction, AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
        expect_same_log(later, legacy);
    }
}

TEST(OwnerBindingApi, bound_goal_and_ready_task_resume_without_new_authority) {
    WorkNode work;
    work.run();
    auto body = WorkNode::create_body(work.instance(agent_a), 0, "timer", 1, {nullptr, 0},
                                      AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
    const auto created = work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    const auto goal_id = created.result.object.id;
    for (unsigned round = 0; round < 100 && work.goal(goal_id).last_task_id == 0U; ++round) {
        const auto report = work.node().step(1);
        work.node().sim().complete_storage();
        advance_deadline(work.node(), report);
    }
    const auto before = work.goal(goal_id);
    ASSERT_NE(before.last_task_id, 0U);
    ASSERT_TRUE(work.strategy().seen.empty());
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_NONE);
    const auto task = work.task(before.last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.hold_status, AYNI_OK);
    EXPECT_EQ(task.hold_reason, AYNI_REASON_NONE);
    work.node().step(1);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.strategy().seen.back().snapshot.task_id, before.last_task_id);
    EXPECT_EQ(work.goal(goal_id).occurrences, before.occurrences);
    EXPECT_EQ(work.goal(goal_id).last_task_id, before.last_task_id);
}

// C-LIFE-021 / ADR-0038: recovery preserves user lifecycle and revision CAS.
static void expect_recovered_goal_controls(ayni_goal_lifecycle_t stored_lifecycle) {
    WorkNode work;
    work.run();
    const auto created = work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
    ASSERT_EQ(created.status, AYNI_OK);
    const auto id = created.result.object.id;
    if (stored_lifecycle == AYNI_GOAL_PAUSED) {
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, id).status, AYNI_OK);
    }
    const auto stored = work.goal(id);
    store_through(work.node(), stored.commit_seq);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    const auto before = work.goal(id);
    ASSERT_EQ(before.lifecycle, stored_lifecycle);
    ASSERT_EQ(before.hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(before.revision, stored.revision);
    EXPECT_EQ(before.commit_seq, stored.commit_seq);
    const auto kind =
        stored_lifecycle == AYNI_GOAL_ACTIVE ? AYNI_INTENT_GOAL_PAUSE : AYNI_INTENT_GOAL_RESUME;
    expect_refused(work.change(kind, id, before.revision + 1U), AYNI_REVISION_CONFLICT,
                   AYNI_REASON_GOAL_REVISION, AYNI_OBJECT_GOAL, id);
    ASSERT_EQ(work.change(kind, id, before.revision).status, AYNI_OK);
    EXPECT_EQ(work.goal(id).lifecycle,
              stored_lifecycle == AYNI_GOAL_ACTIVE ? AYNI_GOAL_PAUSED : AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(id).revision, before.revision + 1U);
}

TEST(OwnerBindingApi, recovered_active_goal_accepts_ordinary_pause) {
    expect_recovered_goal_controls(AYNI_GOAL_ACTIVE);
}

TEST(OwnerBindingApi, recovered_user_paused_goal_accepts_ordinary_resume) {
    expect_recovered_goal_controls(AYNI_GOAL_PAUSED);
}

TEST(OwnerBindingApi, recovery_retains_a_missing_goal_type_diagnostic) {
    WorkNode work;
    work.run();
    const auto created = work.create(WorkNode::create_body(work.instance(agent_a), 0, "timer"));
    ASSERT_EQ(created.status, AYNI_OK);
    store_through(work.node(), created.result.commit_seq);
    work.node().on_create(
        [](ayni_kernel_t *kernel) { register_folder_node(kernel, owner_desc()); });
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.goal(created.result.object.id).hold_reason, AYNI_REASON_GOAL_TYPE_UNKNOWN);
    EXPECT_NE(work.goal(created.result.object.id).hold_reason, AYNI_REASON_LEGACY_UNCONFIRMED);
    work.node().step();
    EXPECT_TRUE(work.strategy().seen.empty());
}

TEST(OwnerBindingApi, new_child_of_a_recovered_authorized_goal_can_derive_and_decide_work) {
    WorkNode work;
    work.run();
    const auto parent = work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
    ASSERT_EQ(parent.status, AYNI_OK);
    store_through(work.node(), parent.result.commit_seq);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    ASSERT_EQ(work.goal(parent.result.object.id).hold_reason, AYNI_REASON_NONE);
    work.reopen_session();

    const auto child =
        work.create(WorkNode::create_body(work.instance(agent_a), parent.result.object.id, "timer",
                                          1, {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(child.status, AYNI_OK);
    EXPECT_EQ(work.goal(child.result.object.id).parent_goal_id, parent.result.object.id);
    ASSERT_TRUE(work.strategy().seen.empty());
    for (unsigned round = 0; round < 10; ++round) {
        work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
        work.node().step();
        work.node().sim().complete_storage();
    }
    EXPECT_NE(work.goal(child.result.object.id).last_task_id, 0U);
    EXPECT_EQ(work.goal(child.result.object.id).occurrences, 1U);
    EXPECT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.goal(parent.result.object.id).hold_reason, AYNI_REASON_NONE);
}

TEST(OwnerBindingApi, power_loss_keeps_the_first_binding_and_instances_as_one_batch) {
    for (const auto cut : {CutAppend::lost, CutAppend::torn, CutAppend::stored}) {
        SCOPED_TRACE(static_cast<int>(cut));
        WorkNode work;
        enter_running_before_startup_is_stored(work.node());
        const auto before = work.agent(agent_a);
        ASSERT_GT(before.commit_seq, work.node().info().durable_seq);
        for (unsigned round = 0; round < 100 && work.node().sim().pending_storage() == 0U;
             ++round) {
            const auto report = work.node().step(1);
            advance_deadline(work.node(), report);
        }
        ASSERT_NE(work.node().sim().pending_storage(), 0U);
        work.node().power_cut_and_start(cut);
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        const auto bindings = stored_bindings(work.node());
        ASSERT_EQ(bindings.size(), 1U);
        const auto after = work.agent(agent_a);
        EXPECT_EQ(bindings[0].owner.principal_id, owner_id);
        EXPECT_EQ(bindings[0].owner.binding_revision, owner_binding);
        EXPECT_EQ(bindings[0].commit_seq, after.commit_seq);
        EXPECT_NE(bindings[0].transition_id, 0U);
        EXPECT_EQ(work.agent(agent_b).commit_seq, after.commit_seq);
        if (cut == CutAppend::stored) {
            EXPECT_EQ(after.instance_id, before.instance_id);
        } else {
            EXPECT_NE(after.instance_id, before.instance_id);
        }
        work.reopen_session();
        EXPECT_NE(work.session(), 0U);
    }
}

// ADR-0038: an ownerless store containing only Agent identities has never
// admitted owner-controlled work and may still bind its first owner.
TEST(OwnerBindingApi, an_unbound_agent_only_store_can_bind_its_first_owner) {
    Node source;
    source.on_create([](ayni_kernel_t *kernel) {
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
    });
    source.run();
    ASSERT_TRUE(stored_bindings(source).empty());
    ayni_agent_info_v1 instance{};
    instance.struct_size = sizeof(instance);
    ASSERT_EQ(ayni_get_agent(source.kernel(), agent_a, &instance), AYNI_OK);
    const auto original_id = instance.instance_id;
    WorkNode later;
    later.node().sim().durable_storage() = source.sim().durable_storage();
    later.run();
    ASSERT_EQ(later.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(later.instance(agent_a), original_id);
    const auto bindings = stored_bindings(later.node());
    ASSERT_EQ(bindings.size(), 1U);
    EXPECT_EQ(bindings.front().owner.principal_id, owner_id);
    EXPECT_EQ(bindings.front().owner.binding_revision, owner_binding);
}

} // namespace
