// White-box decision-lease boundaries, C-MEM-004/005 and C-POLICY-001.
// Task admission uses the public ABI; callbacks use the real WorkModel call-out owner.
#include "kernel/service/memory/memory_decision.h"
#include "work_node.h"

namespace {
using namespace ayni::test;
using ayni::kernel::KernelTestProbe;
using ayni::kernel::MemoryDecision;
using ayni::kernel::OwnerClock;

class MemoryDecisionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        work.node().config().memory_capacity = 3;
        work.node().config().memory_body_max_bytes = static_cast<uint32_t>(staging.size());
        work.node().config().memory_dependency_capacity = 3;
        work.strategy().answer = cannot_decide;
        work.run();
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1, {}, AYNI_TRIGGER_ONCE));
        ASSERT_EQ(created.status, AYNI_OK);
        work.node().step(1);
        const auto task = work.goal(created.result.object.id).last_task_id;
        ASSERT_NE(task, 0U);
        auto &model = KernelTestProbe::work(work.kernel());
        ASSERT_TRUE(model.tasks.find(task, task_index));
    }
    MemoryDecision decision() {
        return {KernelTestProbe::work(work.kernel()), task_index, work.node().info().durable_seq,
                OwnerClock{&work.node().sim(),
                           [](void *context) {
                               return static_cast<ayni::hosts::sim::Host *>(context)->now_us();
                           }},
                staging.data()};
    }
    ayni_memory_search_v1 query() {
        return {
            sizeof(ayni_memory_search_v1), AYNI_MEMORY_PRIVATE, work.instance(agent_a), 0, {}, {}};
    }
    ayni_memory_proposal_v1 proposal() {
        ayni_memory_proposal_v1 result{};
        result.struct_size = sizeof(result);
        result.source = AYNI_MEMORY_DERIVED;
        result.step_key = text("remember");
        result.body = text("candidate");
        return result;
    }
    WorkNode work;
    uint32_t task_index = 0;
    std::array<uint8_t, 64> staging{};
};

TEST_F(MemoryDecisionTest, inactive_or_closed_leases_preserve_every_output) {
    auto lease = decision();
    const auto *view = lease.view();
    ayni_memory_info_v1 info{};
    info.struct_size = sizeof(info);
    info.memory_id = 99;
    char body = 'x';
    uint32_t size = 99;
    const auto search = query();
    const auto candidate = proposal();
    const auto check = [&] {
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                  AYNI_INVALID_STATE);
        EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, &info, 1, &size),
                  AYNI_INVALID_STATE);
        EXPECT_EQ(view->propose(view->context, &candidate), AYNI_INVALID_STATE);
        EXPECT_EQ(info.memory_id, 99U);
        EXPECT_EQ(body, 'x');
        EXPECT_EQ(size, 99U);
    };
    check(); // Live lease, outside its callback.
    lease.close();
    KernelTestProbe::work(work.kernel()).call_out(check); // Closed lease, inside a callback.
    check();
}

TEST_F(MemoryDecisionTest, malformed_callback_arguments_do_not_poison_a_valid_empty_search) {
    auto lease = decision();
    const auto *view = lease.view();
    ayni_memory_info_v1 info{};
    info.struct_size = sizeof(info);
    info.memory_id = 99;
    char body = 'x';
    uint32_t size = 99;
    auto search = query();
    const auto candidate = proposal();
    KernelTestProbe::work(work.kernel()).call_out([&] {
        EXPECT_EQ(view->read(nullptr, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, nullptr, &body, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, nullptr, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, nullptr),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->search(nullptr, &search, AYNI_MEMORY_CONTEXT, &info, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->search(view->context, nullptr, AYNI_MEMORY_CONTEXT, &info, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, nullptr, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, &info, 1, nullptr),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->propose(nullptr, &candidate), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(view->propose(view->context, nullptr), AYNI_INVALID_ARGUMENT);
        info.struct_size = 0;
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        info.struct_size = sizeof(info) + 1;
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                  AYNI_UNSUPPORTED_VERSION);
        info.struct_size = sizeof(info);
        EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                  AYNI_NOT_FOUND);
        search.struct_size = 0;
        EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, &info, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        search = query();
        EXPECT_EQ(view->search(view->context, &search, UINT32_MAX, &info, 1, &size),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(info.memory_id, 99U);
        EXPECT_EQ(size, 99U);
        EXPECT_EQ(body, 'x');
        EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, nullptr, 0, &size),
                  AYNI_OK);
        EXPECT_EQ(size, 0U);
    });
    EXPECT_EQ(lease.validate(), AYNI_OK);
    EXPECT_EQ(lease.dependencies().count, 0U);
}

TEST_F(MemoryDecisionTest, proposal_failures_remain_sticky_across_all_callback_capabilities) {
    struct Invalid {
        const char *name;
        void (*change)(ayni_memory_proposal_v1 &);
        ayni_status_t status = AYNI_INVALID_ARGUMENT;
    };
    static const std::array<char, 65> oversized{};
    const Invalid invalid[] = {
        {"short", [](auto &v) { v.struct_size = 0; }},
        {"future", [](auto &v) { ++v.struct_size; }, AYNI_UNSUPPORTED_VERSION},
        {"reserved", [](auto &v) { v.reserved = 1; }},
        {"user", [](auto &v) { v.source = AYNI_MEMORY_USER; }},
        {"source", [](auto &v) { v.source = UINT32_MAX; }},
        {"step", [](auto &v) { v.step_key = {}; }},
        {"body", [](auto &v) { v.body = {}; }},
        {"null_body", [](auto &v) { v.body = {nullptr, 1}; }},
        {"utf8", [](auto &v) { v.body = {"\xff", 1}; }},
        {"size", [](auto &v) { v.body = {oversized.data(), oversized.size()}; },
         AYNI_CAPACITY_EXCEEDED},
        {"call_identity", [](auto &v) { v.source_call_id = 1; }},
        {"tags", [](auto &v) { v.tag_count = 1; }},
        {"keywords", [](auto &v) { v.keyword_count = 1; }},
    };
    const auto valid = proposal();
    for (const auto &fault : invalid) {
        SCOPED_TRACE(fault.name);
        auto lease = decision();
        const auto *view = lease.view();
        auto changed = valid;
        fault.change(changed);
        auto search = query();
        ayni_memory_info_v1 info{};
        info.struct_size = sizeof(info);
        info.memory_id = 99;
        char body = 'x';
        uint32_t size = 99;
        KernelTestProbe::work(work.kernel()).call_out([&] {
            EXPECT_EQ(view->propose(view->context, &changed), fault.status);
            EXPECT_EQ(view->propose(view->context, &valid), fault.status);
            EXPECT_EQ(view->read(view->context, 0, AYNI_MEMORY_CONTEXT, &info, &body, 1, &size),
                      fault.status);
            EXPECT_EQ(view->search(view->context, &search, AYNI_MEMORY_CONTEXT, &info, 1, &size),
                      fault.status);
        });
        EXPECT_EQ(lease.validate(), fault.status);
        EXPECT_EQ(info.memory_id, 99U);
        EXPECT_EQ(size, 99U);
        EXPECT_EQ(body, 'x');
        EXPECT_EQ(KernelTestProbe::work(work.kernel()).memories.size(), 0U);
    }
    auto lease = decision();
    const auto *view = lease.view();
    KernelTestProbe::work(work.kernel()).call_out([&] {
        EXPECT_EQ(view->propose(view->context, &valid), AYNI_OK);
    });
    const auto &task = KernelTestProbe::work(work.kernel()).tasks.at(task_index);
    const auto intent = policy_intent(AYNI_POLICY_WRITE_MEMORY, task.revision, "remember");
    EXPECT_EQ(lease.check_proposal(intent), AYNI_OK);
    EXPECT_EQ(lease.validate(), AYNI_OK);
    KernelTestProbe::work(work.kernel()).call_out([&] {
        EXPECT_EQ(view->propose(view->context, &valid), AYNI_CAPACITY_EXCEEDED);
    });
    EXPECT_EQ(lease.validate(), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(KernelTestProbe::work(work.kernel()).memories.size(), 0U);
}
} // namespace
