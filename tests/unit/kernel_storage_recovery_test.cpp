// ADR-0027: public behavior across retries, bounded maintenance and migration.
#include "durability/batch/batch_codec.h"
#include "kernel_node.h"
#include "work_node.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
using namespace ayni::test;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StoragePort;

// These scenarios use goals with no clock trigger. Advancing a reported
// deadline therefore drives only storage, never invents application work.
void settle(WorkNode &work) {
    for (unsigned round = 0; round < 10000; ++round) {
        const auto report = work.node().step(1);
        const auto posts = work.node().sim().complete_storage();
        if (posts != 0U || report.more_ready != 0U) {
            continue;
        }
        if (report.next_deadline_us != UINT64_MAX &&
            report.next_deadline_us > work.node().sim().now_us()) {
            work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
            continue;
        }
        return;
    }
    ADD_FAILURE() << "storage did not settle";
}

ayni_goal_create_v1 folder(WorkNode &work, ayni_bytes_v1 parameters = {nullptr, 0}) {
    return WorkNode::create_body(work.instance(agent_a), 0, "folder", 1, parameters,
                                 AYNI_TRIGGER_NONE);
}

Submitted command(WorkNode &work, const ayni_goal_create_v1 &body, uint64_t id) {
    auto intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.command_id = id;
    return work.submit(intent);
}

// C-COMMIT-008: every retry keeps its logical identity, while the Host may
// already have persisted the first attempt whose result was UNKNOWN.
TEST(StorageRecovery, failed_or_unknown_append_retries_without_duplicate_application) {
    for (const auto fault : {StorageFault::fail, StorageFault::unknown_lost,
                             StorageFault::unknown_persisted, StorageFault::reject}) {
        SCOPED_TRACE(static_cast<int>(fault));
        WorkNode work;
        work.run();
        const auto before = work.node().sim().log().size();
        const auto attempts_before = work.node().sim().storage_attempts().size();
        const auto body = folder(work);
        const auto accepted = command(work, body, 17);
        ASSERT_EQ(accepted.status, AYNI_OK);
        work.node().sim().inject_fault(StoragePort::append, fault);
        settle(work);
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(work.node().info().durable_seq, accepted.result.commit_seq);
        EXPECT_EQ(work.node().sim().log().size(), before + 1U);
        ASSERT_GE(work.node().sim().storage_attempts().size(), attempts_before + 2U);
        const auto &attempts = work.node().sim().storage_attempts();
        EXPECT_NE(attempts[attempts_before].request_id, attempts[attempts_before + 1U].request_id);
        const auto repeated = command(work, body, 17);
        ASSERT_EQ(repeated.status, AYNI_OK);
        EXPECT_EQ(repeated.result.object.id, accepted.result.object.id);
        EXPECT_EQ(repeated.result.commit_seq, accepted.result.commit_seq);
        work.node().reboot();
        work.node().recover();
        work.reopen_session();
        EXPECT_EQ(command(work, body, 17).result.object.id, accepted.result.object.id);
    }
}

// C-COMMIT-008 and ADR-0027: failures pause new transitions but do not hide
// the first result of an already accepted command or roll back its query.
TEST(StorageRecovery, retry_wait_is_readable_and_preserves_command_lookup_before_busy) {
    WorkNode work;
    work.run();
    const auto body = folder(work);
    const auto accepted = command(work, body, 21);
    ASSERT_EQ(accepted.status, AYNI_OK);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::unknown_lost);
    // Drive to the failed completion, without reaching its retry deadline.
    for (unsigned round = 0; round < 20 && !work.kernel()->log.paused(); ++round) {
        const auto report = work.node().step(1);
        work.node().sim().complete_storage();
        if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
            report.next_deadline_us != UINT64_MAX && !work.kernel()->log.paused() &&
            report.next_deadline_us > work.node().sim().now_us()) {
            work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
        }
    }
    ASSERT_TRUE(work.kernel()->log.paused()); // White-box: storage retry wait.
    EXPECT_EQ(work.goal(accepted.result.object.id).goal_id, accepted.result.object.id);
    const auto repeated = command(work, body, 21);
    EXPECT_EQ(repeated.status, AYNI_OK);
    EXPECT_EQ(repeated.result.commit_seq, accepted.result.commit_seq);
    const auto refused = command(work, body, 22);
    EXPECT_EQ(refused.status, AYNI_BUSY);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_STORAGE_BACKLOG);
    const auto report = work.node().step();
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_GT(report.next_deadline_us, work.node().sim().now_us());
    settle(work);
    EXPECT_EQ(work.node().info().durable_seq, accepted.result.commit_seq);
}

// C-STORE-011: rollover is automatic, leaves the current queryable state,
// and keeps accepted command identity across a new Kernel instance.
TEST(StorageRecovery, automatic_snapshot_rotates_log_and_keeps_current_command_window) {
    WorkNode work;
    work.node().config().storage_buffer_bytes = 0;
    work.node().config().storage_log_max_bytes = 32768;
    work.run();
    uint64_t last_goal = 0;
    uint64_t last_command = 0;
    for (uint64_t index = 1; index <= 180; ++index) {
        const auto body = folder(work);
        const auto accepted = command(work, body, index);
        ASSERT_EQ(accepted.status, AYNI_OK) << index;
        last_goal = accepted.result.object.id;
        last_command = index;
        settle(work);
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        if (index != 180U) {
            ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, last_goal, work.goal(last_goal).revision)
                          .status,
                      AYNI_OK);
            settle(work);
        }
    }
    ASSERT_FALSE(work.node().sim().log().empty());
    EXPECT_GT(work.node().sim().log().front().commit_seq, 1U);
    const auto before = work.goal(last_goal);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(work.goal(last_goal).revision, before.revision);
    EXPECT_EQ(work.goal(last_goal).commit_seq, before.commit_seq);
    const auto repeated = command(work, folder(work), last_command);
    EXPECT_EQ(repeated.status, AYNI_OK);
    EXPECT_EQ(repeated.result.object.id, last_goal);
}

} // namespace
