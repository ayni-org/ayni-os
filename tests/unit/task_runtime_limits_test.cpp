// C-RUN-001/002/003: normal admission facts, independent holds, owner raise,
// exact retry identity and codec. Call dispatch/recovery campaigns remain MA-008.
#include "journal_test_support.h"
#include "kernel/service/command/command_records.h"
#include "kernel/service/command/command_task_limit.h"
#include "snapshot_test_support.h"
#include "work/task/task_records.h"
#include "work_node.h"

#include <array>
#include <gtest/gtest.h>

namespace {
using namespace ayni::test;
using namespace ayni::work;
using namespace ayni::kernel;
constexpr uint32_t raised_limit = 12;
constexpr uint64_t command_id = 80;

class TaskRuntimeLimits : public ::testing::Test {
  protected:
    std::vector<uint64_t> arena =
        std::vector<uint64_t>(TaskTable::storage_size({2, 2, 8}) / sizeof(uint64_t) + 1U);
    TaskTable tasks{arena.data(), {2, 2, 8}};
    uint64_t seq = 0;
    ayni::foundation::TransitionStamp stamp() {
        ++seq;
        return {seq, seq};
    }
    void SetUp() override {
        ASSERT_TRUE(tasks.insert({10, 20, 0, 0, 1, 1}, 30, stamp()));
        ASSERT_TRUE(tasks.insert({11, 21, 0, 1, 1, 1}, 31, stamp()));
    }
    void exhaust() {
        for (uint32_t n = 0; n < AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT; ++n) {
            tasks.accept_model_call(0, stamp());
        }
        tasks.hold_model_calls(0, stamp());
    }
};

TEST_F(TaskRuntimeLimits, limit_hold_keeps_progress_and_other_tasks_ready) {
    ASSERT_TRUE(tasks.add_step(0, text("first"), stamp()));
    exhaust();
    EXPECT_EQ(TaskTable::effective_hold_reason(tasks.at(0)), AYNI_REASON_TASK_MODEL_CALL_LIMIT);
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.at(0).step_count, 1U);
    EXPECT_TRUE(tasks.has_held());
    uint32_t next = 0;
    ASSERT_TRUE(tasks.next_ready(UINT64_MAX, next));
    EXPECT_EQ(tasks.at(next).task_id, 31U);
    const auto revision = tasks.at(0).revision;
    ASSERT_EQ(tasks.check_model_limit_raise(0, revision, raised_limit), AYNI_OK);
    tasks.raise_model_limit(0, raised_limit, stamp());
    EXPECT_FALSE(tasks.has_held());
    EXPECT_EQ(tasks.at(0).model_calls.used, AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT);
    EXPECT_EQ(tasks.at(0).step_count, 1U);
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_READY);
}

TEST_F(TaskRuntimeLimits, ordinary_pause_resume_does_not_reset_count_or_limit_hold) {
    exhaust();
    ASSERT_TRUE(TaskTable::allows(tasks.at(0), TaskChange::pause));
    tasks.apply(0, TaskChange::pause, stamp());
    ASSERT_TRUE(TaskTable::allows(tasks.at(0), TaskChange::resume));
    tasks.apply(0, TaskChange::resume, stamp());
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.at(0).model_calls.used, AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT);
    tasks.apply(0, TaskChange::pause, stamp());
    tasks.raise_model_limit(0, raised_limit, stamp());
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.at(0).hold_status, AYNI_OK);
    EXPECT_FALSE(tasks.has_held()); // User pause is not a failed policy decision.
}

TEST_F(TaskRuntimeLimits, raising_preserves_policy_and_compatibility_holds) {
    exhaust();
    tasks.hold(0, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT, stamp());
    tasks.raise_model_limit(0, raised_limit, stamp());
    EXPECT_TRUE(tasks.has_held());
    EXPECT_EQ(TaskTable::effective_hold_status(tasks.at(0)), AYNI_CAPACITY_EXCEEDED);
    // White-box owner hook: a releasable runtime hold must not hide compatibility evidence.
    tasks.accept_model_call(1, stamp());
    tasks.hold_restored(1, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
    tasks.raise_model_limit(1, raised_limit, stamp());
    EXPECT_EQ(TaskTable::effective_hold_reason(tasks.at(1)), AYNI_REASON_STRATEGY_CHANGED);
}

TEST_F(TaskRuntimeLimits, runtime_codec_preserves_complete_runtime_and_default_facts) {
    exhaust();
    const auto &task = tasks.at(0);
    std::array<uint8_t, task_state_record_size> bytes{};
    write_task_state_record(task, bytes.data());
    TaskStateRecord decoded{};
    ASSERT_TRUE(read_task_state_record(bytes.data(), bytes.size(), decoded));
    EXPECT_EQ(decoded.state.model_calls, task.model_calls);
    const auto &legacy = tasks.at(1);
    write_task_state_record(legacy, bytes.data());
    ASSERT_TRUE(read_task_state_record(bytes.data(), task_state_record_size, decoded));
    EXPECT_EQ(decoded.state.model_calls, TaskModelCalls{});
}

uint64_t create_task(WorkNode &work) {
    auto body =
        WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1, {}, AYNI_TRIGGER_ONCE);
    const auto goal = work.create(body);
    EXPECT_EQ(goal.status, AYNI_OK);
    work.node().step(1);
    return work.goal(goal.result.object.id).last_task_id;
}

TEST(TaskLimitApi, owner_raise_is_exact_idempotent_and_preserves_policy_hold) {
    WorkNode work;
    work.run();
    const auto task_id = create_task(work);
    ASSERT_NE(task_id, 0U);
    work.strategy().answer = cannot_decide;
    work.node().step(1);
    const auto before = work.task(task_id);
    ASSERT_NE(before.hold_status, AYNI_OK);
    ayni_task_raise_model_limit_v1 body{sizeof(body), raised_limit, task_id};
    auto request = work.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &body, before.revision);
    request.command_id = command_id;
    const auto applied = work.submit(request);
    ASSERT_EQ(applied.status, AYNI_OK);
    const auto after = work.task(task_id);
    EXPECT_EQ(after.revision, before.revision + 1U);
    EXPECT_EQ(after.model_call_limit, raised_limit);
    EXPECT_EQ(after.model_calls_used, before.model_calls_used);
    EXPECT_EQ(after.hold_status, before.hold_status);
    EXPECT_EQ(after.hold_reason, before.hold_reason);
    const auto retry = work.submit(request);
    EXPECT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, applied.result.commit_seq);
    EXPECT_EQ(work.task(task_id).revision, after.revision);
    // C-COMMIT-004: the normal storage completion makes this exact command durable.
    constexpr uint32_t storage_rounds = 128;
    for (uint32_t round = 0;
         round < storage_rounds && work.node().info().durable_seq < applied.result.commit_seq;
         ++round) {
        work.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        work.node().step();
        work.node().sim().complete_storage();
    }
    ASSERT_GE(work.node().info().durable_seq, applied.result.commit_seq);
    EXPECT_EQ(work.submit(request).result.phase, AYNI_INTENT_DURABLE);
    ++body.model_call_limit;
    EXPECT_EQ(work.submit(request).status, AYNI_IDEMPOTENCY_CONFLICT);
    --body.model_call_limit;
    request.session_id = work.open_session(AYNI_CHANNEL_VOICE);
    EXPECT_EQ(work.submit(request).status, AYNI_DENIED);
    EXPECT_EQ(work.task(task_id).model_call_limit, raised_limit);
}

TEST(TaskLimitApi, validates_cas_hard_ceiling_and_live_state_before_changing_anything) {
    WorkNode work;
    work.node().config().task_model_call_hard_limit = raised_limit;
    work.run();
    const auto id = create_task(work);
    const auto before = work.task(id);
    EXPECT_EQ(before.model_call_hard_limit, raised_limit);
    ayni_task_raise_model_limit_v1 body{sizeof(body), raised_limit, id};
    auto request = work.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &body, before.revision);
    EXPECT_EQ(work.submit(request).status, AYNI_INVALID_ARGUMENT); // No command ID.
    request.command_id = command_id;
    ++body.model_call_limit;
    EXPECT_EQ(work.submit(request).status, AYNI_INVALID_ARGUMENT);
    body.model_call_limit = before.model_call_limit;
    EXPECT_EQ(work.submit(request).status, AYNI_INVALID_ARGUMENT); // No decrease/reset.
    body.model_call_limit = raised_limit;
    ++request.expected_revision;
    EXPECT_EQ(work.submit(request).status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(work.task(id).revision, before.revision);
    ASSERT_EQ(work.cancel_task(id).status, AYNI_OK);
    request.expected_revision = work.task(id).revision;
    EXPECT_EQ(work.submit(request).status, AYNI_INVALID_STATE);
}

TEST(TaskLimitApi, startup_ceiling_can_reduce_initial_limit_without_affecting_steps) {
    WorkNode work;
    work.node().config().task_model_call_hard_limit = 1;
    work.run();
    const auto info = work.task(create_task(work));
    EXPECT_EQ(info.model_call_limit, 1U);
    EXPECT_EQ(info.model_call_hard_limit, 1U);
    EXPECT_EQ(info.model_calls_used, 0U);
}

TEST(TaskLimitCommand, codec_binds_task_revision_and_total_limit_to_exact_result) {
    const TaskLimitCommand input{3, 40, raised_limit};
    std::array<uint8_t, task_limit_command_size> bytes{};
    uint32_t written = 0;
    ASSERT_EQ(encode_task_limit_command(input, bytes.data(), bytes.size(), written), AYNI_OK);
    TaskLimitCommand decoded{};
    ASSERT_TRUE(read_task_limit_command({bytes.data(), written}, decoded));
    EXPECT_EQ(decoded, input);
    CommandMapping mapping{7,
                           command_id,
                           authority_command_digest({bytes.data(), written}),
                           9,
                           AYNI_INTENT_TASK_RAISE_MODEL_LIMIT,
                           {0, input.task_id, input.expected_revision + 1U, AYNI_OBJECT_TASK},
                           written};
    ASSERT_TRUE(valid_exact_mapping(mapping, {bytes.data(), written}));
    std::array<uint8_t, command_record_size + task_limit_command_size> record{};
    ASSERT_TRUE(
        write_command_record(mapping, 0, {bytes.data(), written}, record.data(), record.size()));
    CommandMapping restored{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    EXPECT_EQ(mapping.kind, AYNI_INTENT_TASK_RAISE_MODEL_LIMIT);
    ASSERT_TRUE(read_command_record(record.data(), record.size(), restored, floor, canonical));
    EXPECT_EQ(restored.object.revision, mapping.object.revision);
    EXPECT_EQ(restored.digest, mapping.digest);
}

// C-RUN-003 / C-STORE-007: a snapshot has no preceding state to replay. Its
// exact raise receipt must independently agree with the retained current Task.
TEST(TaskLimitApi, snapshot_raise_receipt_proves_the_same_task_revision_and_ceiling) {
    WorkNode source;
    source.run();
    source.strategy().answer = cannot_decide;
    const auto task_id = create_task(source);
    const auto before = source.task(task_id);
    const ayni_task_raise_model_limit_v1 body{sizeof(body), raised_limit, task_id};
    auto request = source.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &body, before.revision);
    request.command_id = command_id;
    ASSERT_EQ(source.submit(request).status, AYNI_OK);
    const auto original = snapshot_model(source.node());
    const auto restore = [](const ModelSnapshot &snapshot) {
        WorkNode target;
        target.node().config() = snapshot.config;
        return restore_model(snapshot, target.node());
    };
    ASSERT_EQ(restore(original), ayni::durability::BatchCheck::ok);
    auto &tasks = KernelTestProbe::work(source.kernel()).tasks;
    uint32_t index = 0;
    ASSERT_TRUE(tasks.find(task_id, index));
    const auto accepted = tasks.at(index);
    const auto changes = {
        +[](Task &t) { --t.revision; },
        +[](Task &t) { ++t.revision; },
        +[](Task &t) { --t.model_calls.limit; },
        +[](Task &t) { ++t.model_calls.limit; },
        +[](Task &t) { t.model_calls.used = t.model_calls.limit; },
        +[](Task &t) { t.model_calls.blocked = true; },
        +[](Task &t) { t.lifecycle = AYNI_TASK_COMPLETED; },
    };
    for (std::size_t variant = 0; variant < changes.size(); ++variant) {
        SCOPED_TRACE(variant);
        auto candidate = original;
        auto changed = accepted;
        const auto change = *(changes.begin() + variant);
        change(changed);
        uint32_t selected = 0;
        for (auto &record : candidate.records) {
            if (record.header.type != task_state_record_type) {
                continue;
            }
            TaskStateRecord decoded{};
            ASSERT_TRUE(read_task_state_record(record.body.data(), record.header.length, decoded));
            if (decoded.task_id == task_id) {
                ++selected;
                write_task_state_record(changed, record.body.data(), decoded.change);
            }
        }
        ASSERT_EQ(selected, 1U);
        EXPECT_EQ(restore(candidate), ayni::durability::BatchCheck::corrupt);
    }
    // A later ordinary cancellation is legitimate: the original command is historical,
    // and its exact retry result must survive a greater current revision.
    ASSERT_EQ(source.cancel_task(task_id).status, AYNI_OK);
    ASSERT_EQ(restore(snapshot_model(source.node())), ayni::durability::BatchCheck::ok);
}
// C-RUN-003 / C-STORE-009: the owner command changes only the soft ceiling
// and its hold. Every CRC-valid single-bit change to that complete state must
// disagree with either the old Task or its exact management receipt.
TEST(TaskLimitApi, altered_raise_state_cannot_be_repaired_by_a_later_valid_command) {
    WorkNode source;
    source.run();
    source.strategy().answer = cannot_decide;
    const auto task_id = create_task(source);
    source.node().step(1);
    const auto before = source.task(task_id);
    const ayni_task_raise_model_limit_v1 body{sizeof(body), raised_limit, task_id};
    auto request = source.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &body, before.revision);
    request.command_id = command_id;
    const auto accepted = source.submit(request);
    ASSERT_EQ(accepted.status, AYNI_OK);
    for (unsigned round = 0;
         round < 128 && source.node().info().durable_seq < accepted.result.commit_seq; ++round) {
        source.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        source.node().step();
        source.node().sim().complete_storage();
    }
    ASSERT_GE(source.node().info().durable_seq, accepted.result.commit_seq);
    const auto log = source.node().sim().log();
    WorkNode control;
    control.node().config() = source.node().config();
    control.node().sim().log() = log;
    control.node().create_and_start();
    control.node().recover();
    ASSERT_EQ(control.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    uint32_t targets = 0;
    constexpr unsigned bits_per_byte = 8;
    for (std::size_t item = 0; item < log.size(); ++item) {
        const auto batch = read_stored_batch(log[item]);
        if (batch.identity.commit_seq != accepted.result.commit_seq) {
            continue;
        }
        for (std::size_t row = 0; row < batch.records.size(); ++row) {
            const auto &record = batch.records[row];
            if (record.header.type != task_state_record_type) {
                continue;
            }
            TaskStateRecord state_record{};
            ASSERT_TRUE(
                read_task_state_record(record.body.data(), record.header.length, state_record));
            if (state_record.change != TaskRecordChange::raise_limit) {
                continue;
            }
            ++targets;
            ASSERT_FALSE(record.body.empty());
            for (std::size_t at = 0; at < record.body.size(); ++at) {
                for (unsigned bit = 0; bit < bits_per_byte; ++bit) {
                    SCOPED_TRACE(::testing::Message() << at << ':' << bit);
                    auto changed = batch;
                    changed.records[row].body[at] ^= static_cast<uint8_t>(1U << bit);
                    auto candidate = log;
                    candidate[item] = write_stored_batch(changed);
                    WorkNode recovered;
                    recovered.node().config() = source.node().config();
                    recovered.node().sim().log() = std::move(candidate);
                    recovered.node().create_and_start();
                    recovered.node().recover();
                    EXPECT_EQ(recovered.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
                    EXPECT_EQ(recovered.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
                    EXPECT_EQ(KernelTestProbe::work(recovered.kernel()).tasks.size(), 0U);
                }
            }
        }
    }
    ASSERT_EQ(targets, 1U);
    WorkNode smaller;
    smaller.node().config() = source.node().config();
    smaller.node().config().task_model_call_hard_limit = raised_limit - 1;
    smaller.node().sim().log() = log;
    smaller.node().create_and_start();
    smaller.node().recover();
    EXPECT_EQ(smaller.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(smaller.node().info().restriction, AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
}
} // namespace
