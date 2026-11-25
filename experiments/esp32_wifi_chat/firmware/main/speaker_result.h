#pragma once
#include <ayni/calls.h>

namespace wifichat {
// Output evidence belongs to the I2S worker; cancellation alone proves no stop.
enum class SpeakerOutput { none, stopped, unknown };
struct SpeakerEvidence {
    SpeakerOutput output; // none: no samples submitted; stopped: disable/delete/park confirmed.
    bool complete;        // True only when all PCM drained and output is confirmed stopped.
    bool cancelled;       // An explicit cancellation was requested; independent of output facts.
    bool expired;         // Deadline expired; cancellation must not hide this failure class.
};
struct SpeakerResult {
    ayni_call_outcome_t outcome; // Terminal tracking outcome; never inferred from request alone.
    ayni_status_t status;        // Public failure class, not a vendor error.
    ayni_call_effect_t effect;   // Submitted audio knowledge; not an acoustic measurement.
};

// ADR-0115: normal barge-in preserves partial output without fabricating an unknown operation.
inline SpeakerResult speaker_result(const SpeakerEvidence &evidence) {
    if (evidence.output == SpeakerOutput::unknown) {
        return {AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_CALL_EFFECT_UNKNOWN};
    }
    const auto effect = evidence.output == SpeakerOutput::none ? AYNI_CALL_EFFECT_NONE
                        : evidence.complete                    ? AYNI_CALL_EFFECT_COMPLETE
                                                               : AYNI_CALL_EFFECT_PARTIAL;
    if (evidence.cancelled && !evidence.expired) {
        return {AYNI_CALL_CANCELLED, AYNI_OK, effect};
    }
    if (evidence.complete && !evidence.expired) {
        return {AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE};
    }
    return {AYNI_CALL_FAILED, evidence.expired ? AYNI_EXPIRED : AYNI_NOT_READY, effect};
}
} // namespace wifichat
