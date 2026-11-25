#include "speaker_result.h"
#include <gtest/gtest.h>

namespace {
using namespace wifichat;

// ADR-0115 / C-CALL-009: a stopped partial cancellation is a known terminal effect.
TEST(SpeakerResult, interrupted_output_keeps_partial_effect_after_confirmed_stop) {
    const auto result = speaker_result({SpeakerOutput::stopped, false, true, false});
    EXPECT_EQ(result.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(result.status, AYNI_OK);
    EXPECT_EQ(result.effect, AYNI_CALL_EFFECT_PARTIAL);
}

// ADR-0115: no samples is distinct from partial audio; late cancel cannot erase full output.
TEST(SpeakerResult, cancellation_preserves_none_and_complete_output_evidence) {
    EXPECT_EQ(speaker_result({SpeakerOutput::none, false, true, false}).effect,
              AYNI_CALL_EFFECT_NONE);
    EXPECT_EQ(speaker_result({SpeakerOutput::stopped, true, true, false}).effect,
              AYNI_CALL_EFFECT_COMPLETE);
}

// C-CALL-009: request acceptance without stop proof cannot report known cancellation.
TEST(SpeakerResult, unconfirmed_stop_stays_unknown_and_deadline_keeps_its_cause) {
    const auto unknown = speaker_result({SpeakerOutput::unknown, false, true, false});
    EXPECT_EQ(unknown.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(unknown.status, AYNI_RESULT_UNKNOWN);
    const auto expired = speaker_result({SpeakerOutput::stopped, false, true, true});
    EXPECT_EQ(expired.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(expired.status, AYNI_EXPIRED);
    EXPECT_EQ(expired.effect, AYNI_CALL_EFFECT_PARTIAL);
}
} // namespace
