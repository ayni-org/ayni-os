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
};

class GoalTypeRegistry {
  public:
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

    // type has a valid id and a nonzero version.
    AddResult add(const GoalType &type);

    Lookup find(ayni_string_v1 id, uint32_t version) const;

    // index from a found Lookup.
    const GoalType &at(uint32_t index) const { return types_[index]; }

    // Whether the type at index accepts these parameters: none when it has
    // no validator, otherwise whatever the validator returns AYNI_OK for.
    bool accepts(uint32_t index, ayni_bytes_v1 parameters) const;

  private:
    foundation::BoundedArray<GoalType> types_;
};

} // namespace ayni::catalog

#endif // AYNI_CORE_CATALOG_GOAL_TYPE_GOAL_TYPE_REGISTRY_H
