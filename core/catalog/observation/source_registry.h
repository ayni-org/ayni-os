// Static observation identity/type; live samples belong to knowledge/observation.
#ifndef AYNI_CORE_CATALOG_OBSERVATION_SOURCE_REGISTRY_H
#define AYNI_CORE_CATALOG_OBSERVATION_SOURCE_REGISTRY_H
#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"
#include <ayni/observations.h>

namespace ayni::catalog {
// Copied startup contract; no borrowed Provider context, sequence or live value.
struct ObservationSource {
    CatalogId id;                     // Unique nonempty source ID.
    uint32_t version;                 // Positive exact semantics version.
    ayni_observation_kind_t kind;     // BOOL or I64.
    ayni_observation_origin_t origin; // Trusted measurement/inference classification.
    CatalogId unit;                   // Explicit immutable unit; BOOL uses "bool".
    uint64_t validity_max_us;         // Positive finite duration, never a timestamp.
};
// One finite observation requirement, shared by Scene metadata and knowledge checks.
struct ObservationCondition {
    CatalogId source;    // Exact registered source identity, nonempty.
    uint32_t version;    // Positive semantic version.
    int64_t minimum;     // Inclusive source-unit lower bound.
    int64_t maximum;     // Inclusive upper bound; >= minimum.
    uint64_t max_age_us; // Positive finite age; cannot override source expiry.
};
// Shared typed rules; ABI decoding only checks layout and copies token views.
ayni_status_t validate_source(const ObservationSource &source);
ayni_status_t decode_condition(const ayni_observation_condition_v1 &input,
                               ObservationCondition &out);

// Sole owner of static source descriptions, closed after Kernel startup.
class ObservationSources {
  public:
    static constexpr std::size_t storage_alignment = alignof(ObservationSource);
    static std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<ObservationSource>::storage_size(capacity);
    }
    // Caller arena outlives this registry; null permitted only at zero capacity.
    ObservationSources(void *storage, uint32_t capacity) : entries_(storage, capacity) {}
    // Already decoded in this owner call; duplicate/full registrations preserve state.
    ayni_status_t add_validated(const ObservationSource &source);
    // Exact identity lookup; failure preserves index.
    bool find(ayni_string_v1 id, uint32_t &index) const;
    // Immutable description at index < size().
    const ObservationSource &at(uint32_t index) const { return entries_[index]; }
    // Registered source count, at most configured capacity.
    uint32_t size() const { return entries_.size(); }

  private:
    foundation::BoundedArray<ObservationSource> entries_; // Sole copied descriptions.
};
} // namespace ayni::catalog
#endif
