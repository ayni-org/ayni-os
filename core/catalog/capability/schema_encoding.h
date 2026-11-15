// Immutable machine schema proof codec (ADR-0084). Text/default views borrow
// the stored proof; there is no lookup of a replacement registry during decode.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_SCHEMA_ENCODING_H
#define AYNI_CORE_CATALOG_CAPABILITY_SCHEMA_ENCODING_H
#include "catalog/capability/value_encoding.h"
#include "foundation/workspace.h"

namespace ayni::catalog {
constexpr uint32_t schema_encoding_version = 1;
constexpr uint32_t schema_encoding_bytes_max = 32768;
constexpr uint32_t schema_choice_count_max = 256; // Total members across one schema.
constexpr uint32_t schema_default_node_count_max = AYNI_VALUE_NODE_COUNT_MAX;

// Decoded proof scratch, not persistent state. Copies rebase internal pointers;
// all UTF-8 views still borrow the caller's immutable proof bytes.
struct DecodedSchema {
    // Only active prefixes are initialized/copied; workspace release still erases all bytes.
    DecodedSchema() {}
    DecodedSchema(const DecodedSchema &source);
    DecodedSchema &operator=(const DecodedSchema &source);
    ayni_schema_v1 view() const { return {nodes.data(), count}; }
    std::array<ayni_schema_node_v1, AYNI_SCHEMA_NODE_COUNT_MAX> nodes; // Initialized [0,count).
    std::array<ayni_string_v1, schema_choice_count_max> choices; // Initialized [0,choice_count).
    std::array<ayni_value_node_v1, schema_default_node_count_max>
        default_nodes;                                              // [0,default_count).
    std::array<ayni_value_v1, AYNI_SCHEMA_NODE_COUNT_MAX> defaults; // Optional views in [0,count).
    uint32_t count = 0;         // Valid schema node prefix; zero before decode.
    uint32_t choice_count = 0;  // Valid members, independently bounded.
    uint32_t default_count = 0; // Valid default nodes, not number of default fields.
};

// Canonicalizes object declaration order, enum sets and default values before
// producing a portable complete proof. Failure never writes partial bytes.
ayni_status_t encode_schema(ayni::foundation::Workspace &workspace, const ayni_schema_v1 &schema,
                            uint8_t *out, uint32_t capacity, uint32_t &size);
// The definition was admitted in this call and remains unchanged; no second
// schema validation at the business-contract composition boundary.
ayni_status_t encode_validated_schema(ayni::foundation::Workspace &workspace,
                                      const ayni_schema_v1 &schema, uint8_t *out, uint32_t capacity,
                                      uint32_t &size);
// Only this version's complete canonical body, including every default/schema
// bound. Failure preserves the caller's scratch and all its internal pointers.
ayni_status_t decode_schema(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                            DecodedSchema &out);
// Internal immutable owner bytes only: registration/replay already admitted this exact proof.
// Reads the same bounded layout without repeating schema validation or canonical re-encoding.
ayni_status_t read_admitted_schema(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                                   DecodedSchema &out);
} // namespace ayni::catalog
#endif
