// C-STORE-011: real space exhaustion is distinct from temporary journal
// backpressure, and neither may invalidate an accepted command's result.

#include "work_node.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace ayni::test;

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

} // namespace
