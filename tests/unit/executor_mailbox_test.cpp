// Executor request lanes and answers, ADR-0052. White-box transport tests:
// what Core does with an answer is covered by ManagedExecution.
#include "kernel/completion/executor_mailbox.h"
#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
using namespace ayni::kernel;
constexpr uint32_t executors = 2;

class ExecutorMailboxTest : public ::testing::Test {
  protected:
    ayni::hosts::sim::Host host;
    ayni_host_v1 ports = host.table();
    std::vector<uint64_t> storage =
        std::vector<uint64_t>(ExecutorMailbox::storage_size(executors) / sizeof(uint64_t) + 1U);
    ExecutorMailbox mailbox{storage.data(), executors, ports};

    ayni_executor_completion_t *open(uint32_t executor, ExecutorLane lane, uint64_t id,
                                     ayni_executor_request_t kind, uint64_t deadline_us = 100) {
        return mailbox.open(executor, lane, {{7, id}, kind, 5, deadline_us, false});
    }
    static ayni_executor_response_v1 response(ayni_executor_request_t kind, uint64_t id,
                                              ayni_status_t status = AYNI_OK) {
        ayni_executor_response_v1 out{};
        out.struct_size = sizeof(out);
        out.kind = kind;
        out.request = {7, id};
        out.status = status;
        return out;
    }
};

TEST_F(ExecutorMailboxTest, one_answer_per_request_and_late_or_duplicate_posts_are_refused) {
    auto *target = open(0, ExecutorLane::control, 1, AYNI_EXECUTOR_CHALLENGE);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(open(0, ExecutorLane::control, 2, AYNI_EXECUTOR_CHALLENGE), nullptr);
    ExecutorAnswer answer{};
    EXPECT_FALSE(mailbox.answer(0, ExecutorLane::control, answer));
    EXPECT_EQ(mailbox.next_deadline_us(), 100U);
    EXPECT_FALSE(mailbox.idle());

    auto other = response(AYNI_EXECUTOR_CHALLENGE, 2);
    EXPECT_EQ(ayni_executor_post(target, &other), AYNI_INVALID_STATE);
    auto wrong_kind = response(AYNI_EXECUTOR_STATUS, 1);
    EXPECT_EQ(ayni_executor_post(target, &wrong_kind), AYNI_INVALID_STATE);

    auto nonce = response(AYNI_EXECUTOR_CHALLENGE, 1);
    nonce.output.nonce = {1, 42};
    const auto wakes = host.wake_count();
    std::thread producer([&] { EXPECT_EQ(ayni_executor_post(target, &nonce), AYNI_OK); });
    producer.join();
    EXPECT_EQ(host.wake_count(), wakes + 1U);
    EXPECT_EQ(ayni_executor_post(target, &nonce), AYNI_INVALID_STATE);
    ASSERT_TRUE(mailbox.answer(0, ExecutorLane::control, answer));
    EXPECT_EQ(answer.status, AYNI_OK);
    EXPECT_EQ(answer.output.nonce.nonce, 42U);
    EXPECT_EQ(mailbox.next_deadline_us(), UINT64_MAX); // Answered, so nothing to time out.

    mailbox.close(0, ExecutorLane::control);
    EXPECT_TRUE(mailbox.idle());
    EXPECT_FALSE(mailbox.answer(0, ExecutorLane::control, answer));
    EXPECT_EQ(ayni_executor_post(target, &nonce), AYNI_INVALID_STATE);
    // The same address serves the next request; the old answer cannot name it.
    EXPECT_EQ(open(0, ExecutorLane::control, 3, AYNI_EXECUTOR_CHALLENGE), target);
    EXPECT_EQ(ayni_executor_post(target, &nonce), AYNI_INVALID_STATE);
    EXPECT_FALSE(mailbox.answer(0, ExecutorLane::control, answer));
}

TEST_F(ExecutorMailboxTest, a_stop_lane_answers_while_other_lanes_wait) {
    ASSERT_NE(open(0, ExecutorLane::control, 1, AYNI_EXECUTOR_RENEW, 300), nullptr);
    ASSERT_NE(open(0, ExecutorLane::status, 2, AYNI_EXECUTOR_STATUS, 200), nullptr);
    ASSERT_NE(open(1, ExecutorLane::status, 3, AYNI_EXECUTOR_STATUS, 150), nullptr);
    auto *stop = open(0, ExecutorLane::stop, 4, AYNI_EXECUTOR_STOP, 400);
    ASSERT_NE(stop, nullptr);
    EXPECT_EQ(mailbox.next_deadline_us(), 150U);
    auto stopped = response(AYNI_EXECUTOR_STOP, 4);
    ASSERT_EQ(ayni_executor_post(stop, &stopped), AYNI_OK);
    ExecutorAnswer answer{};
    EXPECT_TRUE(mailbox.answer(0, ExecutorLane::stop, answer));
    EXPECT_FALSE(mailbox.answer(0, ExecutorLane::control, answer));
    EXPECT_FALSE(mailbox.answer(0, ExecutorLane::status, answer));
    EXPECT_FALSE(mailbox.answer(1, ExecutorLane::status, answer));
    ASSERT_NE(mailbox.request(0, ExecutorLane::status), nullptr);
    EXPECT_EQ(mailbox.request(0, ExecutorLane::status)->call_id, 5U);
    EXPECT_EQ(mailbox.request(1, ExecutorLane::stop), nullptr);
}

TEST_F(ExecutorMailboxTest, malformed_answers_and_foreign_targets_change_nothing) {
    auto *target = open(1, ExecutorLane::status, 1, AYNI_EXECUTOR_STATUS);
    ASSERT_NE(target, nullptr);
    auto valid = response(AYNI_EXECUTOR_STATUS, 1);
    EXPECT_EQ(ayni_executor_post(nullptr, &valid), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_executor_post(target, nullptr), AYNI_INVALID_ARGUMENT);
    ayni_executor_completion_t forged = *target;
    EXPECT_EQ(ayni_executor_post(&forged, &valid), AYNI_INVALID_ARGUMENT);
    auto shape = valid;
    shape.struct_size = sizeof(shape) - 1U;
    EXPECT_EQ(ayni_executor_post(target, &shape), AYNI_INVALID_ARGUMENT);
    shape.struct_size = sizeof(shape) + 1U;
    EXPECT_EQ(ayni_executor_post(target, &shape), AYNI_UNSUPPORTED_VERSION);
    for (const ayni_executor_request_t kind : {0U, 5U}) {
        auto unknown = valid;
        unknown.kind = kind;
        EXPECT_EQ(ayni_executor_post(target, &unknown), AYNI_INVALID_ARGUMENT);
    }
    auto reserved = valid;
    reserved.reserved = 1;
    EXPECT_EQ(ayni_executor_post(target, &reserved), AYNI_INVALID_ARGUMENT);
    ExecutorAnswer answer{};
    EXPECT_FALSE(mailbox.answer(1, ExecutorLane::status, answer));

    // A failed answer carries no output, whatever the executor wrote there.
    auto failed = response(AYNI_EXECUTOR_STATUS, 1, AYNI_STALE);
    failed.output.status.target = AYNI_TARGET_SATISFIED;
    ASSERT_EQ(ayni_executor_post(target, &failed), AYNI_OK);
    ASSERT_TRUE(mailbox.answer(1, ExecutorLane::status, answer));
    EXPECT_EQ(answer.status, AYNI_STALE);
    EXPECT_EQ(answer.output.status.target, AYNI_TARGET_UNVERIFIED);

    // A port that refused its request is answered by its refusal, even after a post.
    mailbox.refuse(1, ExecutorLane::status, AYNI_BUSY);
    ASSERT_TRUE(mailbox.answer(1, ExecutorLane::status, answer));
    EXPECT_EQ(answer.status, AYNI_BUSY);
    mailbox.close(1, ExecutorLane::status);
    mailbox.refuse(1, ExecutorLane::status, AYNI_BUSY);
    EXPECT_FALSE(mailbox.answer(1, ExecutorLane::status, answer));
}
} // namespace
