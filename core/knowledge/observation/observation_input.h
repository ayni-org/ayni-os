// One copied input shape for direct owner and asynchronous publications (ADR-0062).
#ifndef AYNI_CORE_KNOWLEDGE_OBSERVATION_INPUT_H
#define AYNI_CORE_KNOWLEDGE_OBSERVATION_INPUT_H
#include "catalog/observation/source_registry.h"
#include <optional>
namespace ayni::knowledge {
// All representation checks precede transport; source semantics remain immutable.
struct ObservationInput {
    catalog::CatalogId source;   // Copied nonempty source identity, never a routing instruction.
    uint32_t source_version = 0; // Exact positive source version.
    uint64_t generation = 0;     // Positive collection incarnation.
    uint64_t sequence = 0;       // Positive and ordered within its generation.
    uint64_t captured_at_us =
        0; // Host monotonic capture time, checked against owner application time.
    uint64_t valid_for_us = 0;              // Positive finite TTL, with checked timestamp addition.
    ayni_observation_quality_t quality = 0; // GOOD or DEGRADED.
    int64_t value = 0;                      // Representation is decided by the trusted source.
    std::optional<ayni_media_ref_v1> media; // Copied opaque metadata; never byte ownership.
};
// Intrinsic scalar and sample/media pairing invariants. The API already validated
// any media reference with its shared validator before copying the complete layout.
ayni_status_t validate_observation_input(const ObservationInput &input);
// Static source rules, shared by owner publication and producer enqueue once.
ayni_status_t check_observation_source(const ObservationInput &input,
                                       const catalog::ObservationSource &source);
// A gap rejects every old generation and every sequence <= through in this generation.
struct ObservationBoundary {
    uint64_t generation = 0; // Positive when present; 0 is never an applied boundary.
    uint64_t through = 0;    // Inclusive sequence frontier; 0 invalidates only older generations.
};
// Lexicographic identity comparison shared by transport and knowledge owners.
bool after_boundary(uint64_t generation, uint64_t sequence, ObservationBoundary boundary);
// Sample order also requires a nondecreasing capture time within the same generation.
bool newer_sample(const ObservationInput &input, ObservationBoundary last,
                  uint64_t last_captured_at_us);
} // namespace ayni::knowledge
#endif
