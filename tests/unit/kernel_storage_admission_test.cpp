// C-STORE-011: real space exhaustion is distinct from temporary journal
// backpressure, and neither may invalidate an accepted command's result.

#include "kernel/storage/storage_budget.h"
#include "work_node.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {

using namespace ayni::test;

// These goals have no clock trigger, so advancing a reported deadline
// drives only storage.
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

ayni_goal_create_v1 folder(WorkNode &work) {
    return WorkNode::create_body(work.instance(agent_a), 0, "folder", 1, {nullptr, 0},
                                 AYNI_TRIGGER_NONE);
}

Submitted create(WorkNode &work, const ayni_goal_create_v1 &body, uint64_t command_id) {
    auto intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.command_id = command_id;
    return work.submit(intent);
}

TEST(StorageAdmission, full_medium_refuses_new_state_but_keeps_the_original_command_result) {
    WorkNode work;
    work.run();
    const auto body = folder(work);
    const auto accepted = create(work, body, 21);
    ASSERT_EQ(accepted.status, AYNI_OK);
    // White-box: refusal must neither apply a transition nor allocate an
    // object, even though the first command has not reached storage yet.
    const uint64_t next_commit = work.kernel()->work.transitions.next_commit_seq();
    const uint32_t goals = work.kernel()->work.goals.size();
    work.node().sim().set_storage_capacity(0);
    const auto repeated = create(work, body, 21);
    EXPECT_EQ(repeated.status, AYNI_OK);
    EXPECT_EQ(repeated.result.object.id, accepted.result.object.id);
    EXPECT_EQ(repeated.result.commit_seq, accepted.result.commit_seq);
    const auto refused = create(work, body, 22);
    EXPECT_EQ(refused.status, AYNI_STORAGE_FAILED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_STORAGE_SPACE);
    EXPECT_EQ(work.kernel()->work.transitions.next_commit_seq(), next_commit);
    EXPECT_EQ(work.kernel()->work.goals.size(), goals);
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.node().sim().set_storage_capacity(UINT64_MAX);
}

TEST(StorageAdmission, an_unmapped_noncommand_also_requires_space_before_applying) {
    WorkNode work;
    work.run();
    const auto body = folder(work);
    work.node().sim().set_storage_capacity(0);
    const auto refused = create(work, body, 0);
    EXPECT_EQ(refused.status, AYNI_STORAGE_FAILED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_STORAGE_SPACE);
    work.node().sim().set_storage_capacity(UINT64_MAX);
    EXPECT_EQ(create(work, body, 0).status, AYNI_OK);
}

TEST(StorageAdmission, a_retained_principal_window_refuses_a_new_row_before_changing_work) {
    WorkNode work;
    work.run();
    // White-box: recovery retained the unregistered former owner's one
    // command row. It must not be overwritten or dereferenced as a new row.
    const ayni_object_ref_v1 old_object{0, 99, 1, AYNI_OBJECT_GOAL};
    work.kernel()->work.commands.insert(
        {owner_id + 1U, 1, 100, 1, AYNI_INTENT_GOAL_CREATE, old_object});
    const uint64_t next_commit = work.kernel()->work.transitions.next_commit_seq();
    const auto refused = create(work, folder(work), 1);
    EXPECT_EQ(refused.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_NONE);
    EXPECT_EQ(refused.error.object.kind, AYNI_OBJECT_PRINCIPAL);
    EXPECT_EQ(refused.error.object.id, owner_id);
    EXPECT_EQ(work.kernel()->work.transitions.next_commit_seq(), next_commit);
    EXPECT_EQ(work.kernel()->work.goals.size(), 0U);
    const ayni::kernel::CommandMapping *retained = nullptr;
    EXPECT_EQ(work.kernel()->work.commands.find(owner_id + 1U, 1, 100, retained),
              ayni::kernel::CommandTable::Lookup::retried);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->object.id, 99U);
}

// ayni_create refuses a log limit that leaves no room to compact the log.
TEST(StorageAdmission, a_log_limit_without_room_to_compact_is_refused) {
    Node node;
    ayni_config_v1 config = node.config();
    // White-box: the smallest limit, which storage_budget_test.cpp checks.
    const uint64_t smallest = ayni::kernel::StorageBudget::log_max_min(capacities_of(config));
    std::size_t size = 0;
    std::size_t align = 0;
    config.storage_log_max_bytes = static_cast<uint32_t>(smallest - 1U);
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_INVALID_ARGUMENT);
    config.storage_log_max_bytes = static_cast<uint32_t>(smallest);
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK);
    config.storage_log_max_bytes = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK);
}

// White-box: the transition that brings the log to its compaction point
// leaves its batch in the journal, so the compaction waits for the journal
// to drain. No new transition starts meanwhile (ADR-0027 decision 4).
bool holds_while_compaction_waits(WorkNode &work) {
    if (!work.kernel()->budget.compaction_due()) {
        return false;
    }
    EXPECT_FALSE(work.kernel()->log.idle());
    EXPECT_FALSE(work.kernel()->maintenance.active());
    const uint64_t next_commit = work.kernel()->work.transitions.next_commit_seq();
    const auto refused = create(work, folder(work), 0);
    EXPECT_EQ(refused.status, AYNI_BUSY);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_STORAGE_BACKLOG);
    EXPECT_EQ(work.kernel()->work.transitions.next_commit_seq(), next_commit);
    settle(work);
    EXPECT_TRUE(work.kernel()->maintenance.done());
    EXPECT_FALSE(work.kernel()->budget.compaction_due());
    EXPECT_EQ(create(work, folder(work), 0).status, AYNI_OK);
    return true;
}

TEST(StorageAdmission, a_pending_compaction_holds_new_work_until_the_journal_drains) {
    WorkNode work;
    work.node().config().storage_buffer_bytes = 0;
    work.node().config().storage_log_max_bytes = 32768;
    work.run();
    for (int index = 0; index < 1000; ++index) {
        const auto created = create(work, folder(work), 0);
        ASSERT_EQ(created.status, AYNI_OK) << index;
        if (holds_while_compaction_waits(work)) {
            return;
        }
        settle(work);
        const uint64_t goal_id = created.result.object.id;
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_OK) << index;
        if (holds_while_compaction_waits(work)) {
            return;
        }
        settle(work);
    }
    FAIL() << "the log never reached its compaction point";
}

} // namespace
