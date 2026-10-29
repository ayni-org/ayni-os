// Portable business-value bodies, owned by catalog semantics (ADR-0084).
// Local record encoding is separate from ABI layout and any future wire IDL.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_VALUE_ENCODING_H
#define AYNI_CORE_CATALOG_CAPABILITY_VALUE_ENCODING_H
#include <array>
#include <ayni/values.h>

namespace ayni::catalog {
constexpr uint32_t value_encoding_version = 1;
constexpr uint32_t value_encoding_header_bytes = 8; // Format/count, contracts/storage/records.md.
constexpr uint32_t value_node_header_bytes = 12;    // Kind/field/end; no ABI padding.

// Decoded node storage belongs to the caller. Text views borrow the complete
// encoded input until that input changes; node count publishes only on success.
struct DecodedValue {
    std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
    uint32_t count = 0; // Zero before successful complete admission.
    ayni_value_v1 view() const { return {nodes.data(), count}; }
};
// Structural encoding, also used for untrusted receipt staging; no schema claim.
// Failure preserves bytes and size except required size on BUFFER_TOO_SMALL.
ayni_status_t encode_value(const ayni_value_v1 &value, uint8_t *out, uint32_t capacity,
                           uint32_t &size);
// Same-boundary admitted tree only: callers already validated/normalized it;
// these share the single layout calculation/writer without repeating shape admission.
ayni_status_t measure_admitted_value(const ayni_value_v1 &value, uint32_t &size);
ayni_status_t encode_admitted_value(const ayni_value_v1 &value, uint8_t *out, uint32_t capacity,
                                    uint32_t &size);
// Complete shape decoder. Failure preserves out, with no partial published tree.
ayni_status_t decode_value(ayni_bytes_v1 body, DecodedValue &out);
// Exact normalized trees, with only active fields compared and text compared by bytes.
bool same_value(const ayni_value_v1 &left, const ayni_value_v1 &right);
// Stronger persisted-value admission: object order and floating zero are canonical.
bool canonical_value(const ayni_value_v1 &value);
} // namespace ayni::catalog
#endif
