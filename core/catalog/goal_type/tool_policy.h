// Immutable tool policy copied into a Goal, separate from current Grant state.
#ifndef AYNI_CORE_CATALOG_GOAL_TYPE_TOOL_POLICY_H
#define AYNI_CORE_CATALOG_GOAL_TYPE_TOOL_POLICY_H
#include "catalog/catalog_id.h"
#include "foundation/record_body.h"
#include <array>
#include <ayni/ayni.h>
namespace ayni::catalog {
// Stable selection; registration indices and metadata lifetimes are excluded.
struct ToolReference {
    CatalogId binding_id;         // Exact Binding ID; empty only in an unused array entry.
    uint32_t binding_version = 0; // Positive in the policy's used prefix.
    ayni_tool_role_t role = AYNI_TOOL_ROLE_CAPABILITY; // Optional direct Shell selection.
};
// Goal-owned policy ceiling. Being listed never grants permission to execute.
struct ToolPolicy {
    ayni_tool_authorization_t authorization = AYNI_TOOLS_REQUIRED; // Explicit required/lazy mode.
    uint32_t count = 0; // Used refs, 0..GOAL_TYPE_TOOL_COUNT_MAX; zero is an empty policy.
    std::array<ToolReference, AYNI_GOAL_TYPE_TOOL_COUNT_MAX> tools{}; // Distinct Binding IDs.
    bool valid() const; // Shared admission, including one unambiguous Shell role.
    bool
    operator==(const ToolPolicy &other) const; // Exact frozen mode, ordering, roles and selections.
    bool contains(ayni_string_v1 id, uint32_t version) const; // Exact policy selection only.
};
// Portable policy bytes, embedded in GOAL records (contracts/storage/records.md).
constexpr uint32_t tool_policy_header_bytes = 8;    // Authorization/count u32.
constexpr uint32_t tool_reference_fixed_bytes = 12; // Version/role/ID-length u32.
constexpr uint32_t tool_policy_bytes_max =
    tool_policy_header_bytes +
    AYNI_GOAL_TYPE_TOOL_COUNT_MAX * (tool_reference_fixed_bytes + CatalogId::max_size);
// A valid closed directory cannot contain more distinct policy IDs than Bindings.
constexpr uint32_t tool_policy_capacity_bytes(uint32_t bindings) {
    return tool_policy_header_bytes +
           (bindings < AYNI_GOAL_TYPE_TOOL_COUNT_MAX ? bindings : AYNI_GOAL_TYPE_TOOL_COUNT_MAX) *
               (tool_reference_fixed_bytes + CatalogId::max_size);
}
uint32_t tool_policy_size(const ToolPolicy &policy); // Complete admitted used prefix.
void write_tool_policy(const ToolPolicy &policy, foundation::BodyWriter &out);
bool read_tool_policy(foundation::BodyReader &input, ToolPolicy &out); // Failure preserves out.
} // namespace ayni::catalog
#endif
