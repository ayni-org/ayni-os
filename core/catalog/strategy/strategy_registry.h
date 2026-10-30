// Registered strategies (contracts/host/policy_interface.md, ADR-0022). A
// strategy is known by its catalog id, and only one version of an id is
// registered at a time: goal types name strategies by id, and a strategy is
// replaced only across a restart (C-POLICY-010). Registration is closed once
// the kernel leaves CONFIGURING, so an index stays valid for the kernel's
// life.

#ifndef AYNI_CORE_CATALOG_STRATEGY_STRATEGY_REGISTRY_H
#define AYNI_CORE_CATALOG_STRATEGY_STRATEGY_REGISTRY_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"

#include <ayni/ayni.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

// One accepted old writer's inclusive version interval (ADR-0056).
struct CheckpointReader {
    CatalogId id;             // Nonempty writer identity; owned by this registration.
    uint32_t min_version = 0; // Positive lower bound when used.
    uint32_t max_version = 0; // Inclusive upper bound, >= min_version.
};

// Owns immutable strategy registration and its explicit compatibility promise.
struct Strategy {
    CatalogId id;
    uint32_t version = 0;
    void *ctx = nullptr;
    ayni_decide_fn decide = nullptr;
    uint32_t max_intents_per_call = 0;
    uint32_t max_checkpoint_bytes = 0; // its private checkpoints' largest size
    uint32_t reader_count = 0;         // Used prefix of readers; zero accepts only exact self.
    std::array<CheckpointReader, AYNI_CHECKPOINT_READER_COUNT_MAX> readers{};
    uint32_t (*ready)(void *, uint64_t) = nullptr; // Optional bounded external readiness query.

    // Whether this implementation promises to read this nonempty old writer.
    bool accepts_checkpoint(ayni_string_v1 writer, uint32_t writer_version) const;
};

class StrategyRegistry {
  public:
    enum class AddResult { added, duplicate, full };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Strategy>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Strategy>::storage_size(capacity);
    }

    // storage as for BoundedArray<Strategy>.
    StrategyRegistry(void *storage, uint32_t capacity) : strategies_(storage, capacity) {}

    // strategy has a valid id, a nonzero version, a decide function and at
    // least one intent per call; an id already registered is a duplicate,
    // whatever its version.
    AddResult add(const Strategy &strategy);

    // Returns false when no strategy has the id.
    bool find(const CatalogId &id, uint32_t &out_index) const;

    // index from find().
    const Strategy &at(uint32_t index) const { return strategies_[index]; }

  private:
    foundation::BoundedArray<Strategy> strategies_;
};

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_STRATEGY_STRATEGY_REGISTRY_H
