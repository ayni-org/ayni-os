// C-LIFE-030 / ADR-0057: copying enumeration exposes exactly retained identities.
#include "work_node.h"
namespace {
using namespace ayni::test;
TEST(KernelQuery, lists_retained_goals_and_tasks_without_partial_output) {
    WorkNode work;
    work.run();
    auto goal = WorkNode::create_body(work.instance(agent_a), 0, "timer", 1, {}, AYNI_TRIGGER_ONCE);
    const auto first = work.create(goal);
    ASSERT_EQ(first.status, AYNI_OK);
    uint64_t ids[2] = {99, 98};
    uint32_t count = 77;
    EXPECT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_GOAL, nullptr, 0, &count),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(count, 1U);
    EXPECT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_GOAL, ids, 2, &count), AYNI_OK);
    EXPECT_EQ(count, 1U);
    EXPECT_EQ(ids[0], first.result.object.id);
    EXPECT_EQ(ids[1], 98U);
    for (unsigned i = 0; i < 100; ++i) {
        work.node().step();
        work.node().sim().complete_storage();
        work.node().sim().advance_us(1000);
    }
    ASSERT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_TASK, ids, 2, &count), AYNI_OK);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(work.task(ids[0]).goal_id, first.result.object.id);
    EXPECT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_CALL, nullptr, 0, &count), AYNI_OK);
    EXPECT_EQ(count, 0U);
}
TEST(KernelQuery, refuses_unknown_kinds_and_preserves_outputs_outside_query_lifecycle) {
    WorkNode work;
    work.node().create();
    uint32_t count = 55;
    EXPECT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_GOAL, nullptr, 0, &count),
              AYNI_INVALID_STATE);
    EXPECT_EQ(count, 55U);
    EXPECT_EQ(ayni_list_objects(work.kernel(), AYNI_OBJECT_MEMORY, nullptr, 0, &count),
              AYNI_UNSUPPORTED);
    EXPECT_EQ(count, 55U);
}
} // namespace
