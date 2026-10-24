#include "work/authority/authority_cleanup_records.h"

#include <gtest/gtest.h>

#include <array>

namespace {
using namespace ayni::work;
constexpr uint64_t first = UINT64_C(0x100000010);
constexpr uint64_t last = UINT64_C(0x100000020);
constexpr uint64_t newer = UINT64_C(0x100000030);

// ADR-0038 decision 8: requests during a scan cannot extend its fixed frontier
// or disappear, even if the next request covers exactly the same Goal IDs.
TEST(WorkAuthorityCleanup, fixed_frontier_retains_requests_for_a_second_pass) {
    AuthorityCleanup cleanup;
    EXPECT_FALSE(cleanup.ready());
    cleanup.request(0, {1, 1});
    EXPECT_EQ(cleanup.record().stamp.commit_seq, 1U);
    EXPECT_FALSE(cleanup.ready());
    cleanup.request(last, {1, 1});
    cleanup.visited(first, {2, 2});
    cleanup.request(last, {3, 3});
    EXPECT_EQ(cleanup.record().after_goal_id, first);
    EXPECT_EQ(cleanup.record().through_goal_id, last);
    cleanup.visited(last, {4, 4});
    EXPECT_TRUE(cleanup.ready());
    EXPECT_EQ(cleanup.record().after_goal_id, 0U);
    EXPECT_EQ(cleanup.record().through_goal_id, last);
    EXPECT_EQ(cleanup.record().requested_through, 0U);
    cleanup.visited(last, {5, 5});
    EXPECT_FALSE(cleanup.ready());
    EXPECT_TRUE(valid_authority_cleanup_record(cleanup.record()));
    EXPECT_EQ(cleanup.record().stamp.commit_seq, 5U);
}

// ADR-0038 decision 8: slot reuse/removal may leave no retained Goal at the
// last frontier. Closing that range still starts every coalesced next pass.
TEST(WorkAuthorityCleanup, retained_id_gaps_and_later_requests_survive_codec_round_trip) {
    AuthorityCleanup cleanup;
    cleanup.request(last, {1, 1});
    cleanup.visited(first, {2, 2});
    cleanup.request(newer, {3, 3});
    cleanup.request(last, {4, 4});
    std::array<uint8_t, authority_cleanup_record_size> bytes{};
    write_authority_cleanup_record(cleanup.record(), bytes.data());
    AuthorityCleanupRecord saved{};
    ASSERT_TRUE(read_authority_cleanup_record(bytes.data(), bytes.size(), saved));
    AuthorityCleanup recovered;
    recovered.restore_validated(saved);
    EXPECT_EQ(recovered.record().after_goal_id, first);
    EXPECT_EQ(recovered.record().through_goal_id, last);
    EXPECT_EQ(recovered.record().requested_through, newer);
    recovered.finish({5, 5});
    EXPECT_EQ(recovered.record().through_goal_id, newer);
    EXPECT_EQ(recovered.record().after_goal_id, 0U);
    recovered.visited(newer, {6, 6});
    EXPECT_FALSE(recovered.ready());
}

// ADR-0038 decision 12: parsing never publishes a valid prefix after malformed
// state, incomplete bytes or an unexplained trailing byte.
TEST(WorkAuthorityCleanup, codec_rejects_truncation_and_invalid_state_without_output) {
    AuthorityCleanup cleanup;
    cleanup.request(last, {1, 1});
    std::array<uint8_t, authority_cleanup_record_size + 1U> bytes{};
    write_authority_cleanup_record(cleanup.record(), bytes.data());
    AuthorityCleanupRecord out{};
    out.stamp = {7, 7};
    for (uint32_t size = 0; size <= bytes.size(); ++size) {
        if (size == authority_cleanup_record_size) {
            continue;
        }
        EXPECT_FALSE(read_authority_cleanup_record(bytes.data(), size, out));
        EXPECT_EQ(out.stamp.commit_seq, 7U);
        EXPECT_EQ(out.through_goal_id, 0U);
    }
    for (const auto &bad : {
             AuthorityCleanupRecord{{1, 0}},
             AuthorityCleanupRecord{{1, 1}, AuthorityScanPhase::idle, 0, last, 0},
             AuthorityCleanupRecord{{1, 1}, AuthorityScanPhase::idle, 0, 0, last},
             AuthorityCleanupRecord{{}, AuthorityScanPhase::scanning, 0, last, 0},
             AuthorityCleanupRecord{{1, 1}, AuthorityScanPhase::scanning, last, last, 0},
             AuthorityCleanupRecord{{1, 1}, AuthorityScanPhase::scanning, 0, last, 1},
             AuthorityCleanupRecord{{1, 1}, static_cast<AuthorityScanPhase>(9), 0, last, 0},
         }) {
        EXPECT_FALSE(valid_authority_cleanup_record(bad));
        write_authority_cleanup_record(bad, bytes.data());
        EXPECT_FALSE(
            read_authority_cleanup_record(bytes.data(), authority_cleanup_record_size, out));
        EXPECT_EQ(out.stamp.commit_seq, 7U);
    }
}

} // namespace
