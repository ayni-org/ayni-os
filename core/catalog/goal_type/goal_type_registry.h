// Registered goal type versions (C-POLICY-009). A goal type is known by its
// catalog id and version; several versions of one id coexist, so a goal
// keeps the semantics of the version it was created with. Registration is
// closed once the kernel leaves CONFIGURING (C-LIFE-006), so an index stays
// valid for the kernel's life.

#ifndef AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H
#define AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::catalog {

struct GoalType {
    CatalogId id;
    uint32_t version = 0;
    void *ctx = nullptr;
    ayni_goal_validate_fn validate = nullptr; // null: the type takes no parameters
    uint32_t trigger_kinds = 0;               // AYNI_TRIGGER_BIT of each kind it accepts
    // How long a due occurrence may wait for a task (ADR-0020);
    // AYNI_OCCURRENCE_WAIT_UNBOUNDED: no limit.
    uint64_t occurrence_wait_max_us = 0;
};

class GoalTypeRegistry {
  public:
    // The trigger kinds this library defines (ADR-0018, ADR-0019), as
    // trigger_kinds bits; work/goal schedules their occurrences.
    static constexpr uint32_t defined_trigger_kinds =
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) |
        AYNI_TRIGGER_BIT(AYNI_TRIGGER_DELAY) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL);
    static bool defines_trigger(ayni_trigger_kind_t kind) {
        return kind < 32U && (defined_trigger_kinds & AYNI_TRIGGER_BIT(kind)) != 0U;
    }

    enum class AddResult { added, duplicate, full };

    struct Lookup {
        enum class Kind { found, unknown_id, unknown_version } kind;
        uint32_t index; // when found
    };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<GoalType>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<GoalType>::storage_size(capacity);
    }

    // storage as for BoundedArray<GoalType>.
    GoalTypeRegistry(void *storage, uint32_t capacity) : types_(storage, capacity) {}

    // type has a valid id, a nonzero version and trigger_kinds that is a
    // nonempty subset of defined_trigger_kinds.
    AddResult add(const GoalType &type);

    Lookup find(ayni_string_v1 id, uint32_t version) const;

    // index from a found Lookup.
    const GoalType &at(uint32_t index) const { return types_[index]; }

    // Whether the type at index accepts these parameters: none when it has
    // no validator, otherwise whatever the validator returns AYNI_OK for.
    bool accepts(uint32_t index, ayni_bytes_v1 parameters) const;
    // Whether goals of the type at index may choose trigger.
    bool accepts_trigger(uint32_t index, ayni_trigger_kind_t trigger) const;

  private:
    foundation::BoundedArray<GoalType> types_;
};

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H
