// Portable lossless adapter arguments. This body representation is independent
// of external model JSON and of the native-value representation.
#ifndef AYNI_CORE_CATALOG_CAPABILITY_ARGUMENT_ENCODING_H
#define AYNI_CORE_CATALOG_CAPABILITY_ARGUMENT_ENCODING_H
#include <array>
#include <ayni/arguments.h>
namespace ayni::catalog {
constexpr uint32_t argument_encoding_version = 1;
// Caller-owned nodes; every name/string borrows the immutable encoded body.
struct DecodedArguments {
    std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
    uint32_t count = 0; // Occupied prefix after complete decode; zero before admission.
    ayni_arguments_v1 view() const { return {nodes.data(), count}; }
};
// Too-small changes only size; every other failure preserves bytes and size.
ayni_status_t encode_arguments(const ayni_arguments_v1 &input, uint8_t *out, uint32_t capacity,
                               uint32_t &size);
// Complete shape reader; Schema resolution remains map_arguments' responsibility.
ayni_status_t decode_arguments(ayni_bytes_v1 body, DecodedArguments &out);
} // namespace ayni::catalog
#endif
