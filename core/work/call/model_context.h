// Disclosure evidence is separate from execution authority and private memory.
#ifndef AYNI_CORE_WORK_CALL_MODEL_CONTEXT_H
#define AYNI_CORE_WORK_CALL_MODEL_CONTEXT_H
#include "foundation/kernel_id.h"
#include <array>
#include <ayni/directory.h>
namespace ayni::work {
// Exact scoped sample; source semantics and selected Grant remain with its original Call.
struct ModelSample {
    uint64_t call_id = 0;    // Positive subscription identity in the same Task.
    uint64_t generation = 0; // Positive original opening.
    uint64_t sequence = 0;   // Positive exact latest sample.
    bool operator==(const ModelSample &other) const {
        return call_id == other.call_id && generation == other.generation &&
               sequence == other.sequence;
    }
};
// Call-owned, immutable disclosure ceiling. Policy position meanings are frozen
// by the owning Goal. A restart invalidates the metadata, never action facts.
struct ModelContext {
    uint64_t boot_epoch = 0; // Zero is no metadata dependency; positive is the disclosure boot.
    std::array<uint64_t, AYNI_CAPABILITY_DISCLOSURE_WORDS>
        visible{};             // One bit per policy position.
    uint32_t sample_count = 0; // Used prefix below; zero means no sample dependencies.
    std::array<ModelSample, AYNI_MODEL_SAMPLE_DEPENDENCY_MAX>
        samples{}; // Immutable captured sources.
    // Shared intrinsic shape for live identities and their record reader.
    bool valid() const {
        if (sample_count > samples.size() || (!boot_epoch && sample_count)) {
            return false;
        }
        for (uint32_t at = 0; at < sample_count; ++at) {
            const auto &sample = samples[at];
            if (!foundation::is_kernel_id(sample.call_id) ||
                !foundation::is_kernel_id(sample.generation) || !sample.sequence) {
                return false;
            }
            for (uint32_t previous = 0; previous < at; ++previous) {
                if (samples[previous] == sample) {
                    return false;
                }
            }
        }
        return true;
    }
    bool operator==(const ModelContext &other) const {
        if (boot_epoch != other.boot_epoch || visible != other.visible ||
            sample_count != other.sample_count) {
            return false;
        }
        for (uint32_t at = 0; at < sample_count; ++at) {
            if (!(samples[at] == other.samples[at])) {
                return false;
            }
        }
        return true;
    }
};
inline constexpr uint32_t model_context_record_bytes =
    sizeof(uint64_t) * (1U + AYNI_CAPABILITY_DISCLOSURE_WORDS) + sizeof(uint32_t);
inline constexpr uint32_t model_sample_record_bytes =
    sizeof(uint64_t) * 3U; // Call/opening/sequence, records.md.
} // namespace ayni::work
#endif
