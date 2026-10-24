// White-box normal lifecycle tests for the ActionRecord owner, C-COMMIT-006/007,
// C-CALL-008/009/010 and C-LIFE-010. No dispatch, journal or recovery is exercised.
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "work/action/action_record.h"
#include "work/action/action_records.h"

#include <array>
#include <gtest/gtest.h>
#include <type_traits>

namespace {
using namespace ayni::work;
constexpr uint64_t epoch = 5;
static_assert(!std::is_copy_constructible<ActionRecord>::value &&
                  !std::is_copy_assignable<ActionRecord>::value &&
                  !std::is_move_constructible<ActionRecord>::value &&
                  !std::is_move_assignable<ActionRecord>::value,
              "a saved pre-claim object cannot recreate a second submit guard");

ActionStamp stamp(uint64_t seq) {
    return {epoch, {seq, seq}};
}

ActionIdentity identity() {
    ayni::foundation::IdAllocator ids;
    ids.start(epoch);
    ActionIdentity result{};
    EXPECT_TRUE(ids.allocate(result.logical_call_id));
    EXPECT_TRUE(ids.allocate(result.operation_id));
    EXPECT_TRUE(result.binding_id.assign({"mock-a", 6}));
    result.binding_version = 1;
    return result;
}

class ActionRecordTest : public ::testing::Test {
  protected:
    std::optional<ActionRecord> record;
    void SetUp() override {
        ASSERT_EQ(ActionRecord::prepare(identity(), stamp(1), record), AYNI_OK);
    }
    void dispatch() {
        ASSERT_EQ(record->begin_dispatch(stamp(2), 1), AYNI_OK);
        ASSERT_TRUE(record->claim_dispatch(epoch, 2));
    }
};

TEST_F(ActionRecordTest, preparation_and_dispatch_have_separate_durable_barriers) {
    EXPECT_EQ(record->begin_dispatch(stamp(2), 0), AYNI_BUSY);
    EXPECT_EQ(record->fact().phase, ActionPhase::prepared);
    EXPECT_FALSE(record->claim_dispatch(epoch, 1));
    ASSERT_EQ(record->begin_dispatch(stamp(2), 1), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::dispatching);
    EXPECT_EQ(record->fact().effect, ActionEffect::unknown);
    EXPECT_FALSE(record->claim_dispatch(epoch, 1));
    EXPECT_FALSE(record->claim_dispatch(epoch + 1, 2));
    EXPECT_TRUE(record->claim_dispatch(epoch, 2));
    EXPECT_FALSE(record->claim_dispatch(epoch, 2));
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
}

TEST_F(ActionRecordTest, unary_completion_needs_no_progress_receipt_and_preserves_identity) {
    const auto operation = record->fact().identity.operation_id;
    dispatch();
    ASSERT_EQ(record->complete(stamp(3)), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::completed);
    EXPECT_EQ(record->fact().effect, ActionEffect::complete);
    EXPECT_EQ(record->fact().identity.operation_id, operation);
    EXPECT_EQ(record->fact().prepared.transition.commit_seq, 1U);
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
    EXPECT_EQ(record->complete(stamp(4)), AYNI_INVALID_STATE);
    EXPECT_EQ(record->abandon(stamp(4)), AYNI_INVALID_STATE);
    EXPECT_FALSE(record->claim_dispatch(epoch, 4));
}

TEST_F(ActionRecordTest, progress_and_known_failure_retain_partial_effects) {
    dispatch();
    ASSERT_EQ(record->progress(stamp(3)), AYNI_OK);
    ASSERT_EQ(record->progress(stamp(4)), AYNI_OK);
    ASSERT_EQ(record->fail(ActionEffect::partial, stamp(5)), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::failed);
    EXPECT_EQ(record->fact().effect, ActionEffect::partial);
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
    EXPECT_EQ(record->begin_dispatch(stamp(6), 5), AYNI_INVALID_STATE);
}

TEST_F(ActionRecordTest, unary_failure_does_not_require_synthetic_progress) {
    dispatch();
    ASSERT_EQ(record->fail(ActionEffect::none, stamp(3)), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::failed);
    EXPECT_EQ(record->fact().effect, ActionEffect::none);
    EXPECT_EQ(record->fact().evidence, ActionEvidence::provider_result);
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
}

TEST_F(ActionRecordTest, final_check_can_cancel_before_delivery_on_either_side_of_the_barrier) {
    ASSERT_EQ(record->cancel_before_delivery(stamp(2)), AYNI_OK);
    EXPECT_EQ(record->fact().effect, ActionEffect::none);
    EXPECT_EQ(record->fact().phase, ActionPhase::cancelled);
    std::optional<ActionRecord> other;
    ASSERT_EQ(ActionRecord::prepare(identity(), stamp(1), other), AYNI_OK);
    ASSERT_EQ(other->begin_dispatch(stamp(2), 1), AYNI_OK);
    ASSERT_EQ(other->cancel_before_delivery(stamp(3)), AYNI_OK);
    EXPECT_FALSE(other->claim_dispatch(epoch, 3));
    EXPECT_TRUE(ActionRecord::valid(other->fact()));
}

TEST_F(ActionRecordTest, claimed_delivery_requires_submit_rejection_to_record_no_effect) {
    dispatch();
    EXPECT_EQ(record->cancel_before_delivery(stamp(3)), AYNI_INVALID_STATE);
    ASSERT_EQ(record->rejected_not_retained(stamp(3)), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::cancelled);
    EXPECT_EQ(record->fact().effect, ActionEffect::none);
    EXPECT_EQ(record->rejected_not_retained(stamp(4)), AYNI_INVALID_STATE);
}

TEST_F(ActionRecordTest, provider_progress_prevents_later_submit_rejection) {
    dispatch();
    ASSERT_EQ(record->progress(stamp(3)), AYNI_OK);
    EXPECT_EQ(record->rejected_not_retained(stamp(4)), AYNI_INVALID_STATE);
    EXPECT_EQ(record->fact().phase, ActionPhase::in_progress);
    ASSERT_EQ(record->complete(stamp(4)), AYNI_OK);
}

TEST_F(ActionRecordTest, abandonment_never_becomes_an_ordinary_failure_or_a_new_dispatch) {
    dispatch();
    ASSERT_EQ(record->abandon(stamp(3)), AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::result_unknown);
    EXPECT_EQ(record->fact().effect, ActionEffect::unknown);
    EXPECT_EQ(record->fail(ActionEffect::none, stamp(4)), AYNI_INVALID_STATE);
    EXPECT_EQ(record->complete(stamp(4)), AYNI_INVALID_STATE);
    EXPECT_EQ(record->begin_dispatch(stamp(4), 3), AYNI_INVALID_STATE);
    EXPECT_FALSE(record->claim_dispatch(epoch, 4));
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
}

TEST_F(ActionRecordTest, authoritative_completion_reconciles_a_drained_unknown_operation) {
    dispatch();
    ASSERT_EQ(record->abandon(stamp(3)), AYNI_OK);
    ASSERT_EQ(record->reconcile(ActionRecord::QueryConclusion::completed, ActionEffect::complete,
                                ActionRecord::OldAttempt::drained, stamp(4)),
              AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::completed);
    EXPECT_EQ(record->fact().latest.transition.commit_seq, 4U);
    EXPECT_EQ(record->fact().evidence, ActionEvidence::authoritative_result);
    EXPECT_TRUE(ActionRecord::valid(record->fact()));
}

TEST_F(ActionRecordTest,
       authoritative_nonacceptance_seals_the_old_operation_instead_of_reusing_it) {
    dispatch();
    ASSERT_EQ(record->abandon(stamp(3)), AYNI_OK);
    EXPECT_EQ(record->reconcile(ActionRecord::QueryConclusion::not_accepted, ActionEffect::unknown,
                                ActionRecord::OldAttempt::isolated, stamp(4)),
              AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(record->reconcile(ActionRecord::QueryConclusion::not_accepted, ActionEffect::none,
                                ActionRecord::OldAttempt::isolated, stamp(4)),
              AYNI_OK);
    EXPECT_EQ(record->fact().phase, ActionPhase::failed);
    EXPECT_EQ(record->fact().effect, ActionEffect::none);
    EXPECT_EQ(record->fact().evidence, ActionEvidence::authoritative_not_accepted);
    EXPECT_EQ(record->begin_dispatch(stamp(5), 4), AYNI_INVALID_STATE);
    EXPECT_FALSE(record->claim_dispatch(epoch, 5));
}

TEST_F(ActionRecordTest, nonadvancing_or_foreign_writer_stamps_preserve_the_fact) {
    EXPECT_EQ(record->begin_dispatch(stamp(1), 1), AYNI_INVALID_STATE);
    auto foreign = stamp(2);
    foreign.boot_epoch = epoch + 1;
    EXPECT_EQ(record->begin_dispatch(foreign, 1), AYNI_INVALID_STATE);
    EXPECT_EQ(record->fact().phase, ActionPhase::prepared);
    dispatch();
    EXPECT_EQ(record->complete(stamp(2)), AYNI_INVALID_STATE);
    EXPECT_EQ(record->fact().latest.transition.commit_seq, 2U);
    ASSERT_EQ(record->complete(stamp(3)), AYNI_OK);
}

TEST_F(ActionRecordTest, invalid_preparation_and_fact_shapes_are_refused) {
    auto invalid = identity();
    invalid.operation_id = invalid.logical_call_id;
    EXPECT_EQ(ActionRecord::prepare(invalid, stamp(2), record), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(record->fact().latest.transition.commit_seq, 1U);
    auto fact = record->fact();
    fact.effect = ActionEffect::complete;
    EXPECT_FALSE(ActionRecord::valid(fact));
    fact = record->fact();
    fact.phase = ActionPhase::completed;
    fact.effect = ActionEffect::complete;
    EXPECT_FALSE(ActionRecord::valid(fact)); // A completion must follow preparation.
}

TEST_F(ActionRecordTest, preparation_cannot_overwrite_a_live_dispatch_claim) {
    dispatch();
    EXPECT_EQ(ActionRecord::prepare(identity(), stamp(3), record), AYNI_INVALID_STATE);
    EXPECT_EQ(record->fact().phase, ActionPhase::dispatching);
    EXPECT_EQ(record->fact().latest.transition.commit_seq, 2U);
    EXPECT_FALSE(record->claim_dispatch(epoch, 3));
    ASSERT_EQ(record->complete(stamp(3)), AYNI_OK);
}
// Normal codec round trips, not journal/replay or restart tests.
TEST_F(ActionRecordTest, record_codec_keeps_stable_identity_phase_and_evidence) {
    const auto prepared = record->fact();
    dispatch();
    const auto dispatching = record->fact();
    ASSERT_EQ(record->progress(stamp(3)), AYNI_OK);
    const auto progress = record->fact();
    ASSERT_EQ(record->abandon(stamp(4)), AYNI_OK);
    const auto unknown = record->fact();
    ASSERT_EQ(record->reconcile(ActionRecord::QueryConclusion::not_accepted, ActionEffect::none,
                                ActionRecord::OldAttempt::drained, stamp(5)),
              AYNI_OK);
    const ActionFact states[] = {prepared, dispatching, progress, unknown, record->fact()};
    for (const auto &fact : states) {
        std::array<uint8_t, action_state_record_max_size> bytes{};
        const auto size = action_state_record_size(fact);
        write_action_state_record(fact, bytes.data());
        ActionFact decoded{};
        ASSERT_TRUE(read_action_state_record(bytes.data(), size, decoded));
        EXPECT_EQ(decoded.identity.operation_id, fact.identity.operation_id);
        EXPECT_EQ(decoded.identity.logical_call_id, fact.identity.logical_call_id);
        EXPECT_EQ(decoded.identity.binding_version, fact.identity.binding_version);
        EXPECT_TRUE(decoded.identity.binding_id.equals(
            {fact.identity.binding_id.data(), fact.identity.binding_id.size()}));
        EXPECT_EQ(decoded.phase, fact.phase);
        EXPECT_EQ(decoded.effect, fact.effect);
        EXPECT_EQ(decoded.evidence, fact.evidence);
        EXPECT_EQ(decoded.prepared.boot_epoch, fact.prepared.boot_epoch);
        EXPECT_EQ(decoded.prepared.transition.transition_id,
                  fact.prepared.transition.transition_id);
        EXPECT_EQ(decoded.prepared.transition.commit_seq, fact.prepared.transition.commit_seq);
        EXPECT_EQ(decoded.latest.boot_epoch, fact.latest.boot_epoch);
        EXPECT_EQ(decoded.latest.transition.transition_id, fact.latest.transition.transition_id);
        EXPECT_EQ(decoded.latest.transition.commit_seq, fact.latest.transition.commit_seq);
    }
}

TEST_F(ActionRecordTest, record_codec_uses_the_documented_layout_and_exact_length) {
    std::array<uint8_t, action_state_record_max_size> bytes{};
    const auto size = action_state_record_size(record->fact());
    write_action_state_record(record->fact(), bytes.data());
    EXPECT_EQ(size, action_state_record_fixed_size + record->fact().identity.binding_id.size());
    EXPECT_EQ(ayni::foundation::load_le64(bytes.data()), record->fact().identity.operation_id);
    constexpr uint32_t binding_length_at = 80;
    EXPECT_EQ(ayni::foundation::load_le32(bytes.data() + binding_length_at), 6U);
    ActionFact output = record->fact();
    EXPECT_FALSE(read_action_state_record(bytes.data(), size - 1U, output));
    EXPECT_FALSE(read_action_state_record(bytes.data(), size + 1U, output));
    EXPECT_EQ(output.identity.operation_id, record->fact().identity.operation_id);
    EXPECT_EQ(output.phase, ActionPhase::prepared);
}

TEST_F(ActionRecordTest, record_codec_and_live_admission_share_evidence_validation) {
    auto fact = record->fact();
    fact.latest = stamp(2);
    fact.phase = ActionPhase::failed;
    fact.effect = ActionEffect::partial;
    fact.evidence = ActionEvidence::authoritative_not_accepted;
    EXPECT_FALSE(ActionRecord::valid(fact));
    std::array<uint8_t, action_state_record_max_size> bytes{};
    write_action_state_record(record->fact(), bytes.data());
    // The documented evidence slot cannot claim a conclusion while PREPARED.
    constexpr uint32_t evidence_at = 28;
    ayni::foundation::store_le32(bytes.data() + evidence_at,
                                 static_cast<uint32_t>(ActionEvidence::provider_result));
    ActionFact output = record->fact();
    EXPECT_FALSE(read_action_state_record(bytes.data(), action_state_record_size(output), output));
    EXPECT_EQ(output.evidence, ActionEvidence::none);
}
} // namespace
