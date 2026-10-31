// Shared schema/value semantics (ADR-0084). This module owns tree shape and
// primitive bounds, not authorization, JSON syntax or mutable Kernel state.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_VALUE_VALIDATION_H
#define AYNI_CORE_CATALOG_CAPABILITY_VALUE_VALIDATION_H
#include "catalog/capability/value_encoding.h"
#include "foundation/workspace.h"
#include <ayni/values.h>

namespace ayni::catalog {
// A borrowed node representation lets native trees and adapter projections use
// one low-stack shape/scalar validator, without materializing a second tree.
struct ValueNodes {
    const void *context; // Immutable node owner, nonnull through validation.
    uint32_t count;      // Complete node count; zero is invalid, not an empty tree.
    ayni_value_node_v1 (*read)(const void *, uint32_t); // Copies one node, never retains state.
    explicit operator bool() const { return context != nullptr && read != nullptr; }
    ayni_value_node_v1 operator[](uint32_t index) const { return read(context, index); }
};
// Same structural and scalar rules for every borrowed representation.
ayni_status_t validate_value_nodes(const ValueNodes &nodes);
// One kind predicate, reused by shape validation and normalization.
bool value_container(ayni_value_kind_t kind);
// Shape-only admission; does not consult a schema or change output storage.
ayni_status_t validate_value(const ayni_value_v1 &value);
// Static schema admission; defaults are checked through the same normalizer.
ayni_status_t validate_schema(ayni::foundation::Workspace &workspace, const ayni_schema_v1 &schema);
// Validates each static default against its own subtree, with no recursive schema admission.
ayni_status_t validate_schema_defaults(ayni::foundation::Workspace &workspace,
                                       const ayni_schema_v1 &schema);
// Already admitted schema/input, normalized into caller storage atomically.
ayni_status_t normalize_value(ayni::foundation::Workspace &workspace, const ayni_schema_v1 &schema,
                              const ayni_value_v1 &input, ayni_value_buffer_v1 &output);
// Already admitted schema and shape: returns normalized nodes borrowing input/default text.
// The caller encodes or consumes them before either source changes; no byte-size scratch.
ayni_status_t normalize_borrowed_value(ayni::foundation::Workspace &workspace,
                                       const ayni_schema_v1 &schema, const ayni_value_v1 &input,
                                       DecodedValue &output);
// Decode a persisted schema-normalized tree, rejecting omitted defaults or
// noncanonical order/zero. Text continues to borrow the original encoded body.
ayni_status_t read_normalized_value(ayni::foundation::Workspace &workspace,
                                    const ayni_schema_v1 &schema, ayni_bytes_v1 body,
                                    DecodedValue &out);
// Admitted schema field's default, normalized against that exact subtree.
ayni_status_t normalize_schema_default(ayni::foundation::Workspace &workspace,
                                       const ayni_schema_v1 &schema, uint32_t field,
                                       ayni_value_buffer_v1 &output);
// The primitive's active representation and static bounds; containers excluded.
bool scalar_matches(const ayni_schema_node_v1 &schema, const ayni_value_node_v1 &value);
} // namespace ayni::catalog
#endif
