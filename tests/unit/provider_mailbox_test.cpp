// Normal bounded asynchronous delivery, C-PROV-006/C-CALL-012, ADR-0044.
// White-box transport tests: no Provider dispatch or business commit is claimed.
#include "kernel/completion/provider_mailbox.h"
#include <array>
#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {
using namespace ayni::kernel;
using Kind = ProviderMessageKind;
using Ack = ProviderMailbox::Acknowledge;
constexpr uint32_t slots = 2;
constexpr uint32_t body_capacity = 16;
constexpr uint64_t boot_epoch = 7;

class ProviderMailboxTest : public ::testing::Test {
  protected:
    ayni::hosts::sim::Host host;
    ayni_host_v1 ports = host.table();
    std::vector<uint64_t> storage = std::vector<uint64_t>(
        ProviderMailbox::storage_size({slots, body_capacity}) / sizeof(uint64_t) + 1U);
    ProviderMailbox mailbox{storage.data(), {slots, body_capacity}, ports};
    std::array<uint8_t, body_capacity> buffer{};
    uint32_t required = 0;
    ProviderMessage message{};
    ProviderReceipt receipt{};
    ProviderReservation reserve(uint64_t id, uint32_t written_limit = 0) {
        ProviderReservation reservation{};
        EXPECT_EQ(mailbox.reserve({boot_epoch, id}, reservation, written_limit), AYNI_OK);
        return reservation;
    }
    ayni_status_t post(ProviderReservation reservation, Kind kind, uint64_t sequence = 0,
                       ayni_bytes_v1 bytes = {}) {
        return mailbox.post(reservation, {kind, sequence, AYNI_OK, AYNI_REASON_NONE, bytes});
    }
    ayni_status_t read() {
        return mailbox.read_next(buffer.data(), body_capacity, required, message, receipt);
    }
};

TEST_F(ProviderMailboxTest,
       reserves_complete_delivery_capacity_before_submit_and_reuses_only_after_ack) {
    const auto a = reserve(1);
    const auto b = reserve(2);
    ProviderReservation untouched{99, {90, 91}};
    EXPECT_EQ(mailbox.reserve({boot_epoch, 3}, untouched), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(untouched.index, 99U);
    EXPECT_EQ(mailbox.in_flight(), slots);
    ASSERT_EQ(post(a, Kind::completed), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(mailbox.in_flight(), slots); // A read does not release anything.
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    const auto c = reserve(3);
    EXPECT_EQ(c.index, a.index);
    EXPECT_EQ(post(a, Kind::failed), AYNI_INVALID_STATE);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::stale);
    EXPECT_TRUE(mailbox.withdraw(b));
    EXPECT_TRUE(mailbox.withdraw(c));
    EXPECT_EQ(mailbox.in_flight(), 0U);
}

TEST_F(ProviderMailboxTest, retains_cancel_and_terminal_until_the_owner_can_commit_each) {
    const auto reservation = reserve(1);
    const std::array<uint8_t, 3> result{{1, 2, 3}};
    ASSERT_EQ(post(reservation, Kind::cannot_cancel), AYNI_OK);
    ASSERT_EQ(post(reservation, Kind::progress, 1), AYNI_OK);
    ASSERT_EQ(post(reservation, Kind::completed, 0, {result.data(), result.size()}), AYNI_OK);
    for (uint32_t pass = 0; pass < 3; ++pass) {
        ASSERT_EQ(read(), AYNI_OK); // Simulates normal journal backpressure: no ack.
        EXPECT_EQ(message.kind, Kind::cannot_cancel);
    }
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::completed);
    EXPECT_EQ(required, result.size());
    EXPECT_EQ(buffer[0], 1U);
    EXPECT_EQ(buffer[2], 3U);
    EXPECT_TRUE(mailbox.has_pending());
    EXPECT_EQ(mailbox.in_flight(), 1U);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    EXPECT_FALSE(mailbox.has_pending());
    EXPECT_EQ(mailbox.in_flight(), 0U);
}

// C-PROV-003/006: public posting copies business text and independent evidence;
// this transport test does not assert that the consumer has authorized that evidence.
TEST_F(ProviderMailboxTest, public_post_copies_result_metadata_and_rejects_a_reused_attempt) {
    const auto original = reserve(1);
    auto *target = mailbox.completion(original);
    ASSERT_NE(target, nullptr);
    char text[] = "reply";
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = AYNI_CALL_FAILED;
    result.attempt = original.attempt;
    result.status = AYNI_RESULT_UNKNOWN;
    result.effect = AYNI_CALL_EFFECT_PARTIAL;
    result.query_result = AYNI_QUERY_RESULT_FOUND;
    result.result = {text, 5};
    ASSERT_EQ(ayni_provider_post(target, &result), AYNI_OK);
    text[0] = 'x';
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::failed);
    EXPECT_EQ(message.status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(message.effect, AYNI_CALL_EFFECT_PARTIAL);
    EXPECT_EQ(message.query_result, AYNI_QUERY_RESULT_FOUND);
    EXPECT_EQ(std::memcmp(buffer.data(), "reply", 5), 0);
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    const auto next = reserve(2);
    EXPECT_EQ(mailbox.completion(original), nullptr);
    EXPECT_EQ(mailbox.completion(next), target);
    EXPECT_EQ(ayni_provider_post(target, &result), AYNI_INVALID_STATE);
    EXPECT_TRUE(mailbox.withdraw(next));
}

// C-VER-003/C-PROV-006: malformed public posting never consumes the reservation.
TEST_F(ProviderMailboxTest, public_post_reports_shape_limits_without_losing_the_terminal_slot) {
    const auto reservation = reserve(1);
    auto *target = mailbox.completion(reservation);
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result) + 1;
    EXPECT_EQ(ayni_provider_post(target, &result), AYNI_UNSUPPORTED_VERSION);
    result.struct_size = sizeof(result);
    result.attempt = reservation.attempt;
    EXPECT_EQ(ayni_provider_post(target, &result), AYNI_INVALID_ARGUMENT);
    result.outcome = AYNI_CALL_COMPLETED;
    result.effect = AYNI_CALL_EFFECT_COMPLETE;
    result.result = {"body longer than configured lane", body_capacity + 1};
    EXPECT_EQ(ayni_provider_post(target, &result), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(mailbox.in_flight(), 1U);
    EXPECT_FALSE(mailbox.has_pending());
    result.result = {nullptr, 0};
    ASSERT_EQ(ayni_provider_post(target, &result), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest, progress_is_copied_and_old_ack_cannot_erase_newer_progress) {
    const auto reservation = reserve(1);
    std::array<uint8_t, 1> producer{{4}};
    ASSERT_EQ(post(reservation, Kind::progress, 1, {producer.data(), producer.size()}), AYNI_OK);
    producer[0] = 5;
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(buffer[0], 4U);
    const auto first = receipt;
    ASSERT_EQ(post(reservation, Kind::progress, 2, {producer.data(), producer.size()}), AYNI_OK);
    EXPECT_EQ(buffer[0], 4U); // Caller copy is stable while the producer posts.
    EXPECT_EQ(mailbox.acknowledge(first), Ack::superseded);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.sequence, 2U);
    EXPECT_EQ(buffer[0], 5U);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    EXPECT_FALSE(mailbox.has_pending());
    EXPECT_EQ(post(reservation, Kind::progress, 1), AYNI_OK); // Older progress is ignored.
    EXPECT_FALSE(mailbox.has_pending());
    EXPECT_FALSE(mailbox.withdraw(reservation)); // Once any result exists, submit cannot deny it.
    ASSERT_EQ(post(reservation, Kind::cancelled), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest,
       terminal_supersedes_progress_without_overwriting_accepted_cancel_control) {
    const auto reservation = reserve(1);
    ASSERT_EQ(post(reservation, Kind::progress, 1), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    const auto progress = receipt;
    ASSERT_EQ(post(reservation, Kind::cancel_unknown), AYNI_OK);
    ASSERT_EQ(post(reservation, Kind::abandoned), AYNI_OK);
    EXPECT_EQ(mailbox.acknowledge(progress), Ack::superseded);
    EXPECT_EQ(post(reservation, Kind::progress, 2), AYNI_INVALID_STATE);
    EXPECT_EQ(post(reservation, Kind::cannot_cancel), AYNI_INVALID_STATE);
    EXPECT_EQ(post(reservation, Kind::completed), AYNI_INVALID_STATE);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::cancel_unknown);
    const auto cancel = receipt;
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    EXPECT_EQ(mailbox.acknowledge(cancel), Ack::stale);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::abandoned);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest, pending_attempt_does_not_block_another_attempts_delivery) {
    const auto a = reserve(1);
    const auto b = reserve(2);
    ASSERT_EQ(post(a, Kind::completed), AYNI_OK);
    ASSERT_EQ(post(b, Kind::failed), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(receipt.reservation.attempt.request_id, 1U);
    // A remains pending; next scan rotates to B without requiring A to commit.
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(receipt.reservation.attempt.request_id, 2U);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(receipt.reservation.attempt.request_id, 1U);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest, small_buffers_and_invalid_posts_preserve_pending_data) {
    const auto reservation = reserve(1);
    const std::array<uint8_t, body_capacity + 1U> large{};
    EXPECT_EQ(post(reservation, Kind::completed, 0, {large.data(), large.size()}),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(post(reservation, Kind::progress), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(post(reservation, Kind::completed, 1), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(post(reservation, Kind::cannot_cancel, 0, {large.data(), 1}), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(post(reservation, static_cast<Kind>(99)), AYNI_INVALID_ARGUMENT);
    EXPECT_FALSE(mailbox.has_pending());
    ASSERT_EQ(post(reservation, Kind::completed, 0, {large.data(), body_capacity}), AYNI_OK);
    message.sequence = 99;
    EXPECT_EQ(mailbox.read_next(nullptr, 0, required, message, receipt), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(message.sequence, 99U);
    EXPECT_EQ(required, body_capacity);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.payload.size, body_capacity);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest,
       acknowledged_cancel_is_not_accepted_twice_and_withdraw_cannot_discard_results) {
    const auto reservation = reserve(1);
    ASSERT_EQ(post(reservation, Kind::cannot_cancel), AYNI_OK);
    EXPECT_FALSE(mailbox.withdraw(reservation));
    ASSERT_EQ(read(), AYNI_OK);
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    EXPECT_EQ(post(reservation, Kind::cancel_unknown), AYNI_INVALID_STATE);
    EXPECT_FALSE(mailbox.has_pending());
    EXPECT_EQ(mailbox.in_flight(), 1U);
    ASSERT_EQ(post(reservation, Kind::failed), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest, concurrent_progress_and_control_posts_preserve_both_channels) {
    const auto reservation = reserve(1);
    constexpr uint64_t updates = 1000;
    std::thread progress([&] {
        for (uint64_t sequence = 1; sequence <= updates; ++sequence) {
            EXPECT_EQ(post(reservation, Kind::progress, sequence), AYNI_OK);
        }
    });
    std::thread cancel([&] { EXPECT_EQ(post(reservation, Kind::cannot_cancel), AYNI_OK); });
    progress.join();
    cancel.join();
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::cannot_cancel);
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.kind, Kind::progress);
    EXPECT_EQ(message.sequence, updates);
    ASSERT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    ASSERT_EQ(post(reservation, Kind::completed), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
}

TEST_F(ProviderMailboxTest, attempt_identity_is_monotone_even_after_a_slot_was_withdrawn) {
    auto reservation = reserve(2);
    EXPECT_TRUE(mailbox.withdraw(reservation));
    EXPECT_EQ(mailbox.reserve({boot_epoch, 1}, reservation), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(mailbox.reserve({boot_epoch + 1U, 2}, reservation), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(mailbox.reserve({0, 3}, reservation), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(mailbox.reserve({boot_epoch + 1U, UINT64_MAX}, reservation), AYNI_OK);
    EXPECT_TRUE(mailbox.withdraw(reservation));
    EXPECT_EQ(mailbox.reserve({boot_epoch + 1U, 0}, reservation), AYNI_INVALID_ARGUMENT);
}
} // namespace

namespace {
// ADR-0054: a typed body is encoded into its reserved lane under the lock and
// keeps its kind; a writer never serves progress, an attempt that takes no
// written body, or one over the lane or the attempt's own bound.
TEST_F(ProviderMailboxTest, written_terminal_keeps_its_kind_and_lane_bound) {
    struct Fill {
        static void write(const void *context, uint8_t *out, uint32_t size) {
            for (uint32_t at = 0; at < size; ++at) {
                out[at] = *static_cast<const uint8_t *>(context);
            }
        }
    };
    const uint8_t value = 0x5A;
    const PayloadWriter writer{&value, Fill::write};
    const auto text_only = reserve(1);
    const auto reservation = reserve(2, 4);
    ProviderMessage written{Kind::completed, 0, AYNI_OK, AYNI_REASON_NONE, {nullptr, 4}};
    written.body = ayni::work::CallResultKind::tool_calls;
    written.writer = &writer;
    written.kind = Kind::progress;
    written.sequence = 1;
    EXPECT_EQ(mailbox.post(reservation, written), AYNI_INVALID_ARGUMENT);
    written.kind = Kind::completed;
    written.sequence = 0;
    written.payload.size = body_capacity + 1U;
    EXPECT_EQ(mailbox.post(reservation, written), AYNI_CAPACITY_EXCEEDED);
    written.payload.size = 5;
    EXPECT_EQ(mailbox.post(reservation, written), AYNI_CAPACITY_EXCEEDED);
    written.payload.size = 4;
    EXPECT_EQ(mailbox.post(text_only, written), AYNI_INVALID_ARGUMENT);
    const uint8_t stray = 1;
    written.payload = {&stray, 4};
    EXPECT_EQ(mailbox.post(reservation, written), AYNI_INVALID_ARGUMENT);
    EXPECT_FALSE(mailbox.has_pending());
    written.payload = {nullptr, 4};
    ASSERT_EQ(mailbox.post(reservation, written), AYNI_OK);
    ASSERT_EQ(read(), AYNI_OK);
    EXPECT_EQ(message.body, ayni::work::CallResultKind::tool_calls);
    EXPECT_EQ(message.writer, nullptr);
    ASSERT_EQ(required, 4U);
    EXPECT_EQ(buffer[0], value);
    EXPECT_EQ(buffer[3], value);
    EXPECT_EQ(mailbox.acknowledge(receipt), Ack::consumed);
    EXPECT_TRUE(mailbox.withdraw(text_only));
}
} // namespace
