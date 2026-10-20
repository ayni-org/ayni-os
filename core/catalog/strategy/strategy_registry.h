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

#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

struct Strategy {
    CatalogId id;
    uint32_t version = 0;
    void *ctx = nullptr;
    ayni_decide_fn decide = nullptr;
    uint32_t max_intents_per_call = 0;
    uint32_t max_checkpoint_bytes = 0; // its private checkpoints' largest size
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
