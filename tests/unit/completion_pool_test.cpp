// Completion slots (MA-003; C-HOST-004, C-HOST-006, C-HOST-018, ADR-0015):
// back-pressure when slots run out, exactly-once application, attempt
// matching across slot reuse, and posts that never depend on queue space.

#include "kernel/completion/completion_pool.h"
#include "kernel/ingress_lock.h"
#include "kernel/kernel_state.h"

#include <ayni/ayni.h>
#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ayni::kernel::CompletionPool;
using ayni::kernel::IssueResult;

constexpr uint64_t boot_epoch = 7;

// A Host port that answers with a fixed submit result and remembers what it
// was given, as a storage Host would before its I/O thread posts.
struct FakePort {
    ayni_submit_result_t answer = AYNI_SUBMIT_ACCEPTED;
    bool post_before_answer = false;
    std::vector<std::pair<ayni_attempt_ref_v1, ayni_completion_t *>> accepted;
    int calls = 0;

    static ayni_submit_result_t call(void *context, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion);
};

ayni_completion_result_v1 result_for(ayni_attempt_ref_v1 attempt, uint64_t value) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = 1;
    result.attempt = attempt;
    result.value = value;
    return result;
}

ayni_submit_result_t FakePort::call(void *context, ayni_attempt_ref_v1 attempt,
                                    ayni_completion_t *completion) {
    FakePort &port = *static_cast<FakePort *>(context);
    ++port.calls;
    if (port.post_before_answer) {
        const ayni_completion_result_v1 result = result_for(attempt, 0);
        EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    }
    port.accepted.emplace_back(attempt, completion);
    return port.answer;
}

struct Applied {
    std::vector<ayni_completion_result_v1> results;

    static void handle(void *context, const ayni_completion_result_v1 &result) {
        static_cast<Applied *>(context)->results.push_back(result);
    }
};

class CompletionPoolTest : public ::testing::Test {
  protected:
    void make_pool(uint32_t slots) {
        storage_.assign(CompletionPool::storage_size(slots) / sizeof(uint64_t) + 1U, 0);
        pool_.emplace(storage_.data(), slots, host_);
    }

    IssueResult issue() {
        return pool_->issue(boot_epoch, &Applied::handle, &applied_, &FakePort::call, &port_);
    }

    ayni::hosts::sim::Host sim_;
    ayni_host_v1 host_ = sim_.table();
    std::vector<uint64_t> storage_;
    std::optional<CompletionPool> pool_;
    FakePort port_;
    Applied applied_;
};

TEST_F(CompletionPoolTest, exhausted_slots_return_busy_without_calling_the_port) {
    make_pool(2);
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(issue(), IssueResult::busy);
    EXPECT_EQ(port_.calls, 2);
    EXPECT_EQ(pool_->in_flight(), 2U);
    EXPECT_EQ(pool_->counters().rejected_busy, 1U);

    // Applying one result frees its slot for the next request.
    const ayni_completion_result_v1 result = result_for(port_.accepted[0].first, 0);
    ASSERT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_OK);
    EXPECT_EQ(pool_->apply_posted(8), 1U);
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(port_.calls, 3);
}

TEST_F(CompletionPoolTest, attempts_are_named_before_the_port_is_called) {
    make_pool(4);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(port_.accepted[0].first.boot_epoch, boot_epoch);
    EXPECT_EQ(port_.accepted[1].first.boot_epoch, boot_epoch);
    EXPECT_NE(port_.accepted[0].first.request_id, port_.accepted[1].first.request_id);
    EXPECT_NE(port_.accepted[0].second, port_.accepted[1].second);
}

TEST_F(CompletionPoolTest, result_is_applied_once) {
    make_pool(2);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    const auto [attempt, completion] = port_.accepted[0];
    const ayni_completion_result_v1 result = result_for(attempt, 99);
    ASSERT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_INVALID_STATE); // before apply
    EXPECT_EQ(pool_->apply_posted(8), 1U);
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_INVALID_STATE); // after apply
    EXPECT_EQ(pool_->apply_posted(8), 0U);

    ASSERT_EQ(applied_.results.size(), 1U);
    EXPECT_EQ(applied_.results[0].value, 99U);
    EXPECT_EQ(applied_.results[0].attempt.request_id, attempt.request_id);
    EXPECT_EQ(pool_->in_flight(), 0U);
    EXPECT_EQ(pool_->counters().ignored_posts, 2U);
}

// C-HOST-006 counterexample: after reuse, the earlier attempt's repeated OK
// must not count as the later attempt's result.
TEST_F(CompletionPoolTest, late_post_cannot_complete_a_reused_slot) {
    make_pool(1);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    const auto [first_attempt, completion] = port_.accepted[0];
    const ayni_completion_result_v1 first = result_for(first_attempt, 1);
    ASSERT_EQ(ayni_completion_post(completion, &first), AYNI_OK);
    ASSERT_EQ(pool_->apply_posted(8), 1U);

    ASSERT_EQ(issue(), IssueResult::in_flight);
    ASSERT_EQ(port_.accepted[1].second, completion); // the same slot, reused
    const ayni_attempt_ref_v1 second_attempt = port_.accepted[1].first;
    EXPECT_EQ(ayni_completion_post(completion, &first), AYNI_INVALID_STATE);
    EXPECT_EQ(pool_->apply_posted(8), 0U);

    const ayni_completion_result_v1 second = result_for(second_attempt, 2);
    ASSERT_EQ(ayni_completion_post(completion, &second), AYNI_OK);
    ASSERT_EQ(pool_->apply_posted(8), 1U);
    ASSERT_EQ(applied_.results.size(), 2U);
    EXPECT_EQ(applied_.results[1].value, 2U);
}

TEST_F(CompletionPoolTest, post_from_an_earlier_boot_is_ignored) {
    make_pool(1);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    ayni_attempt_ref_v1 stale = port_.accepted[0].first;
    stale.boot_epoch -= 1;
    const ayni_completion_result_v1 result = result_for(stale, 0);
    EXPECT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_INVALID_STATE);
    EXPECT_EQ(pool_->in_flight(), 1U);
}

TEST_F(CompletionPoolTest, rejected_request_frees_the_slot_and_runs_no_handler) {
    make_pool(1);
    port_.answer = AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    EXPECT_EQ(issue(), IssueResult::rejected);
    EXPECT_EQ(pool_->in_flight(), 0U);
    const ayni_completion_result_v1 result = result_for(port_.accepted[0].first, 0);
    EXPECT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_INVALID_STATE);
    EXPECT_EQ(pool_->apply_posted(8), 0U);
    EXPECT_TRUE(applied_.results.empty());
}

TEST_F(CompletionPoolTest, accept_unknown_waits_for_the_post) {
    make_pool(1);
    port_.answer = AYNI_SUBMIT_ACCEPT_UNKNOWN;
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(pool_->in_flight(), 1U);
    EXPECT_EQ(pool_->counters().host_contract_violations, 0U);
}

TEST_F(CompletionPoolTest, undefined_submit_result_counts_as_in_flight) {
    make_pool(1);
    port_.answer = 42;
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(pool_->in_flight(), 1U);
    EXPECT_EQ(pool_->counters().host_contract_violations, 1U);
}

// A Host that posts and then claims it kept no work contradicts itself; the
// posted result exists, so it is applied rather than lost.
TEST_F(CompletionPoolTest, post_before_a_rejection_is_still_applied) {
    make_pool(1);
    port_.answer = AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    port_.post_before_answer = true;
    EXPECT_EQ(issue(), IssueResult::in_flight);
    EXPECT_EQ(pool_->counters().host_contract_violations, 1U);
    EXPECT_EQ(pool_->apply_posted(8), 1U);
    EXPECT_EQ(applied_.results.size(), 1U);
    EXPECT_EQ(pool_->in_flight(), 0U);
}

TEST_F(CompletionPoolTest, apply_respects_max_items_and_posting_order) {
    make_pool(3);
    for (int request = 0; request < 3; ++request) {
        ASSERT_EQ(issue(), IssueResult::in_flight);
    }
    for (const int index : {2, 0, 1}) {
        const auto [attempt, completion] = port_.accepted[static_cast<std::size_t>(index)];
        const ayni_completion_result_v1 result = result_for(attempt, static_cast<uint64_t>(index));
        ASSERT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    }
    EXPECT_EQ(pool_->apply_posted(2), 2U);
    EXPECT_EQ(pool_->apply_posted(2), 1U);
    ASSERT_EQ(applied_.results.size(), 3U);
    EXPECT_EQ(applied_.results[0].value, 2U);
    EXPECT_EQ(applied_.results[1].value, 0U);
    EXPECT_EQ(applied_.results[2].value, 1U);
}

TEST_F(CompletionPoolTest, post_wakes_the_owner_and_no_host_call_runs_under_the_lock) {
    make_pool(2);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    const uint64_t wakes = sim_.wake_count();
    const ayni_completion_result_v1 result = result_for(port_.accepted[0].first, 0);
    ASSERT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_OK);
    EXPECT_EQ(sim_.wake_count(), wakes + 1U);
    EXPECT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_INVALID_STATE);
    EXPECT_EQ(sim_.wake_count(), wakes + 1U); // an ignored post wakes nobody
    pool_->apply_posted(8);
    EXPECT_EQ(sim_.calls_under_lock(), 0U);
}

// Eight threads post 64 results and then repeat each post while the owner
// applies; every request is handled exactly once, every repeat is ignored.
TEST_F(CompletionPoolTest, concurrent_posts_apply_each_result_exactly_once) {
    constexpr uint32_t slots = 64;
    make_pool(slots);
    for (int round = 0; round < 20; ++round) {
        port_.accepted.clear();
        applied_.results.clear();
        for (uint32_t request = 0; request < slots; ++request) {
            ASSERT_EQ(issue(), IssueResult::in_flight);
        }
        std::vector<std::thread> posters;
        for (uint32_t thread = 0; thread < 8U; ++thread) {
            posters.emplace_back([this, thread] {
                for (uint32_t index = thread; index < slots; index += 8U) {
                    const auto [attempt, completion] = port_.accepted[index];
                    const ayni_completion_result_v1 result = result_for(attempt, index);
                    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
                    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_INVALID_STATE);
                }
            });
        }
        // Bounded so that a refused post fails the test instead of hanging it.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        uint32_t handled = 0;
        while (handled < slots && std::chrono::steady_clock::now() < deadline) {
            handled += pool_->apply_posted(5);
            std::this_thread::yield();
        }
        for (std::thread &poster : posters) {
            poster.join();
        }
        ASSERT_EQ(handled, slots);
        EXPECT_EQ(pool_->apply_posted(slots), 0U);
        std::set<uint64_t> requests;
        for (const ayni_completion_result_v1 &result : applied_.results) {
            requests.insert(result.attempt.request_id);
        }
        ASSERT_EQ(requests.size(), slots);
        ASSERT_EQ(pool_->in_flight(), 0U);
    }
    EXPECT_EQ(pool_->counters().ignored_posts, 20U * slots);
    EXPECT_EQ(sim_.calls_under_lock(), 0U);
}

// A Host thread keeps re-posting the first attempt's result while the owner
// reuses the only slot for new attempts. The stale result never completes a
// later attempt, and each attempt is applied once with its own result. Each
// round waits until a stale post has started after the slot was reserved for
// the new attempt, so every round meets the race whatever the scheduler does.
// Under TSan this also catches slot fields written outside the ingress lock.
TEST_F(CompletionPoolTest, stale_posts_racing_slot_reuse_are_ignored) {
    make_pool(1);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    const auto [stale_attempt, slot] = port_.accepted[0];
    const ayni_completion_result_v1 stale = result_for(stale_attempt, 0);
    ASSERT_EQ(ayni_completion_post(slot, &stale), AYNI_OK);
    ASSERT_EQ(pool_->apply_posted(1), 1U);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> stale_posts{0};
    // C++17 lambdas cannot capture a structured binding, hence the copy.
    std::thread stale_poster([&, stale_slot = slot] {
        while (!stop.load()) {
            EXPECT_EQ(ayni_completion_post(stale_slot, &stale), AYNI_INVALID_STATE);
            stale_posts.fetch_add(1);
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    constexpr uint32_t attempts = 500;
    uint32_t rounds = 0;
    while (rounds < attempts) {
        if (issue() != IssueResult::in_flight) {
            ADD_FAILURE() << "issue failed in round " << rounds + 1U;
            break;
        }
        // A post counted after `reserved` may have begun before it; the next
        // one began after, so it met the slot reserved for the new attempt.
        const uint64_t reserved = stale_posts.load();
        while (stale_posts.load() < reserved + 2U && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (stale_posts.load() < reserved + 2U) {
            ADD_FAILURE() << "the stale thread stopped posting in round " << rounds + 1U;
            break;
        }
        ++rounds;
        const auto [attempt, completion] = port_.accepted.back();
        const ayni_completion_result_v1 result = result_for(attempt, rounds);
        EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
        EXPECT_EQ(pool_->apply_posted(1), 1U);
    }
    stop.store(true);
    stale_poster.join();

    ASSERT_EQ(rounds, attempts);
    ASSERT_EQ(applied_.results.size(), attempts + 1U);
    for (uint32_t round = 0; round <= attempts; ++round) {
        EXPECT_EQ(applied_.results[round].attempt.request_id,
                  port_.accepted[round].first.request_id);
        EXPECT_EQ(applied_.results[round].value, round);
    }
    EXPECT_EQ(pool_->counters().ignored_posts, stale_posts.load());
    EXPECT_EQ(pool_->in_flight(), 0U);
}

TEST_F(CompletionPoolTest, zero_outcome_completes_the_slot_and_is_counted) {
    make_pool(1);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    ayni_completion_result_v1 result = result_for(port_.accepted[0].first, 0);
    result.outcome = 0;
    ASSERT_EQ(ayni_completion_post(port_.accepted[0].second, &result), AYNI_OK);
    ASSERT_EQ(pool_->apply_posted(1), 1U);
    EXPECT_EQ(applied_.results[0].outcome, 0U);
    EXPECT_EQ(pool_->in_flight(), 0U);
    EXPECT_EQ(pool_->counters().host_contract_violations, 1U);
}

// The owner may run as soon as it is woken, so a posted result must already
// be applicable when wake is called; otherwise the owner finds nothing, goes
// back to sleep, and the result waits for some unrelated wake. This Host's
// wake applies results on the spot, as an owner woken at that instant would.
struct ApplyOnWakeHost {
    ayni::hosts::sim::Host sim;
    ayni_host_v1 sim_table = sim.table();
    CompletionPool *pool = nullptr;
    uint32_t applied_on_wake = 0;

    ayni_host_v1 table() {
        ayni_host_v1 host = sim_table;
        host.ctx = this;
        host.wake = &wake;
        host.ingress_lock = &lock;
        host.ingress_unlock = &unlock;
        return host;
    }
    static void wake(void *ctx) {
        ApplyOnWakeHost &self = *static_cast<ApplyOnWakeHost *>(ctx);
        self.sim_table.wake(self.sim_table.ctx); // counted if the pool still held the lock
        self.applied_on_wake += self.pool->apply_posted(1);
    }
    static void lock(void *ctx) {
        ApplyOnWakeHost &self = *static_cast<ApplyOnWakeHost *>(ctx);
        self.sim_table.ingress_lock(self.sim_table.ctx);
    }
    static void unlock(void *ctx) {
        ApplyOnWakeHost &self = *static_cast<ApplyOnWakeHost *>(ctx);
        self.sim_table.ingress_unlock(self.sim_table.ctx);
    }
};

TEST(CompletionWake, result_is_applicable_when_the_owner_is_woken) {
    ApplyOnWakeHost probe;
    const ayni_host_v1 host = probe.table();
    std::vector<uint64_t> storage(CompletionPool::storage_size(2) / sizeof(uint64_t) + 1U);
    CompletionPool pool(storage.data(), 2, host);
    probe.pool = &pool;
    FakePort port;
    Applied applied;
    for (uint32_t request = 0; request < 2U; ++request) {
        ASSERT_EQ(pool.issue(boot_epoch, &Applied::handle, &applied, &FakePort::call, &port),
                  IssueResult::in_flight);
        const auto [attempt, completion] = port.accepted.back();
        const ayni_completion_result_v1 result = result_for(attempt, request);
        ASSERT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    }
    EXPECT_EQ(probe.applied_on_wake, 2U);
    EXPECT_EQ(applied.results.size(), 2U);
    EXPECT_EQ(probe.sim.calls_under_lock(), 0U);
}

TEST_F(CompletionPoolTest, completion_post_rejects_invalid_arguments) {
    make_pool(1);
    ASSERT_EQ(issue(), IssueResult::in_flight);
    const auto [attempt, completion] = port_.accepted[0];
    ayni_completion_result_v1 result = result_for(attempt, 0);

    EXPECT_EQ(ayni_completion_post(nullptr, &result), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_completion_post(completion, nullptr), AYNI_INVALID_ARGUMENT);
    result.struct_size = sizeof(result) - 1;
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_INVALID_ARGUMENT);
    result.struct_size = sizeof(result) + 8;
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_UNSUPPORTED_VERSION);

    // Not one of the pool's slots, though it names the pool and an index.
    result.struct_size = sizeof(result);
    ayni_completion copy;
    std::memcpy(&copy, completion, sizeof(copy));
    EXPECT_EQ(ayni_completion_post(&copy, &result), AYNI_INVALID_ARGUMENT);
    ayni_completion orphan{};
    EXPECT_EQ(ayni_completion_post(&orphan, &result), AYNI_INVALID_ARGUMENT);

    EXPECT_EQ(pool_->in_flight(), 1U); // none of these touched the slot
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
}

// MA-003: control confirmations do not depend on event queue space. The
// kernel's own queue is full, yet every completion is posted and applied.
TEST(KernelCompletions, posts_succeed_while_the_event_queue_is_full) {
    ayni::hosts::sim::Host sim;
    const ayni_host_v1 host = sim.table();
    ayni_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 2;
    config.event_body_max_bytes = 8;
    config.completion_slot_count = 4;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_MIN;
    config.agent_capacity = 2;
    config.goal_type_capacity = 2;
    config.goal_capacity = 4;
    config.goal_parameter_max_bytes = 16;
    config.session_capacity = 2;
    config.goal_depth_max = 2;
    config.goal_children_max = 2;
    config.task_capacity = 4;
    std::size_t size = 0;
    std::size_t align = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK);
    std::vector<uint64_t> memory(size / sizeof(uint64_t) + 1U);
    ASSERT_LE(align, alignof(uint64_t));
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(ayni_create(&config, &host, memory.data(), size, &kernel), AYNI_OK);

    ayni::kernel::EventHeader event{};
    event.kind = 3;
    {
        ayni::kernel::IngressGuard guard(kernel->host);
        ASSERT_EQ(kernel->ingress.push(event, nullptr), AYNI_OK);
        ASSERT_EQ(kernel->ingress.push(event, nullptr), AYNI_OK);
        ASSERT_EQ(kernel->ingress.push(event, nullptr), AYNI_BUSY);
    }

    FakePort port;
    Applied applied;
    for (int request = 0; request < 4; ++request) {
        ASSERT_EQ(kernel->completions.issue(boot_epoch, &Applied::handle, &applied, &FakePort::call,
                                            &port),
                  IssueResult::in_flight);
    }
    for (const auto &[attempt, completion] : port.accepted) {
        const ayni_completion_result_v1 result = result_for(attempt, 0);
        EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    }
    EXPECT_EQ(kernel->completions.apply_posted(8), 4U);
    EXPECT_EQ(applied.results.size(), 4U);
    EXPECT_EQ(kernel->completions.in_flight(), 0U);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

} // namespace
