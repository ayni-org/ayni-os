// ADR-0104 / C-COMMIT-004: a bounded prefix remains borrowed until whole-group OK.
#include "kernel/runtime/commit/commit_log.h"
#include <array>
#include <gtest/gtest.h>
#include <vector>
namespace {
struct AppendPort {
    const ayni_storage_batch_v1 *batches = nullptr;
    uint32_t count = 0;
    ayni_attempt_ref_v1 attempt{};
    ayni_completion_t *completion = nullptr;
    static ayni_submit_result_t append(void *context, const ayni_storage_batch_v1 *batches,
                                       uint32_t count, ayni_attempt_ref_v1 attempt,
                                       ayni_completion_t *completion) {
        auto &self = *static_cast<AppendPort *>(context);
        self = {batches, count, attempt, completion};
        return AYNI_SUBMIT_ACCEPTED;
    }
};
TEST(CommitGroups, durable_prefix_advances_only_after_bounded_group_completion) {
    constexpr uint64_t boot = 3;
    constexpr uint32_t buffer_bytes = 4096, batch_max = 128;
    constexpr uint32_t queued = AYNI_STORAGE_APPEND_COUNT_MAX + 1U;
    AppendPort port;
    ayni_host_v1 host{};
    host.ctx = &port;
    host.storage_append_many = AppendPort::append;
    host.monotonic_now_us = [](void *) -> uint64_t { return 0; };
    host.ingress_lock = [](void *) {};
    host.ingress_unlock = [](void *) {};
    host.wake = [](void *) {};
    ayni::kernel::AttemptSequence attempts;
    using ayni::kernel::CompletionPool;
    std::vector<uint64_t> storage(CompletionPool::storage_size(1) / sizeof(uint64_t) + 1U);
    CompletionPool pool(storage.data(), 1, host, attempts);
    ayni::kernel::WriteSchedule writes(0);
    std::array<uint8_t, buffer_bytes> buffer{};
    ayni::kernel::CommitLog log(host, pool, buffer.data(), buffer_bytes, writes);
    log.start(boot, {1, 1, 1}, batch_max);
    for (uint32_t at = 0; at < queued; ++at) {
        ASSERT_TRUE(log.has_room());
        auto batch = log.begin();
        log.queue(batch.finish());
    }
    auto complete = [&] {
        const ayni_completion_result_v1 result{sizeof(result), AYNI_WRITE_OK, port.attempt, 0, 0};
        ASSERT_EQ(ayni_completion_post(port.completion, &result), AYNI_OK);
        EXPECT_EQ(pool.apply_posted(1), 1U);
    };
    ASSERT_TRUE(log.advance(0));
    EXPECT_EQ(port.count, AYNI_STORAGE_APPEND_COUNT_MAX);
    EXPECT_EQ(log.durable_seq(), 0U);
    for (uint32_t at = 0; at < port.count; ++at) {
        ayni::durability::BatchView view{};
        ASSERT_EQ(ayni::durability::read_batch(port.batches[at].encoded.data,
                                               port.batches[at].encoded.size, view),
                  ayni::durability::BatchCheck::ok);
        EXPECT_EQ(view.identity.commit_seq, at + 1U);
    }
    complete();
    EXPECT_EQ(log.durable_seq(), AYNI_STORAGE_APPEND_COUNT_MAX);
    ASSERT_TRUE(log.advance(0));
    EXPECT_EQ(port.count, 1U);
    EXPECT_EQ(port.batches[0].commit_seq, queued);
    complete();
    EXPECT_EQ(log.durable_seq(), queued);
    EXPECT_TRUE(log.idle());
}
} // namespace
