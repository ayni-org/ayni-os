#include "model_tools.h"
#include "protocol.h"
#include <cstring>
namespace usbchat {
namespace {
constexpr size_t call_header_bytes = 8, node_header_bytes = 12, batch_header_bytes = 8;
constexpr size_t name_length_offset = 2, node_count_offset = 4, flags_offset = 6;
constexpr size_t node_end_offset = 4, node_name_offset = 8, node_value_offset = 10;
constexpr uint32_t batch_version = 1;
uint32_t u16(const uint8_t *data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8U);
}
} // namespace
bool ModelTools::decode_call(const uint8_t *data, size_t bytes, uint32_t index) {
    if (bytes < call_header_bytes) {
        return false;
    }
    const auto id_bytes = u16(data), name_bytes = u16(data + name_length_offset);
    const auto node_count = u16(data + node_count_offset);
    if (!id_bytes || id_bytes > AYNI_MODEL_TOOL_ID_BYTES_MAX || !name_bytes ||
        name_bytes > AYNI_CATALOG_ID_MAX_BYTES || !node_count || node_count > nodes_per_tool ||
        u16(data + flags_offset)) {
        return false;
    }
    size_t pos = call_header_bytes;
    auto take = [&](size_t size, ayni_string_v1 &out) {
        if (size > bytes - pos) {
            return false;
        }
        out = {reinterpret_cast<const char *>(data + pos), uint32_t(size)};
        pos += size;
        return true;
    };
    auto &call = calls_[index];
    call = {};
    call.struct_size = sizeof(call);
    call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
    if (!take(id_bytes, call.protocol_id) || !take(name_bytes, call.function_name)) {
        return false;
    }
    for (uint32_t at = 0; at < node_count; ++at) {
        if (bytes - pos < node_header_bytes) {
            return false;
        }
        auto &node = nodes_[index][at];
        node = {};
        node.kind = get32(data + pos);
        node.subtree_end = get32(data + pos + node_end_offset);
        const auto name = u16(data + pos + node_name_offset);
        const auto value = u16(data + pos + node_value_offset);
        pos += node_header_bytes;
        ayni_string_v1 raw{};
        if (!take(name, node.name) || !take(value, raw)) {
            return false;
        }
        if (node.kind == AYNI_ARGUMENT_BOOL) {
            if (raw.size != 1 || (raw.data[0] != '0' && raw.data[0] != '1')) {
                return false;
            }
            node.value.boolean = raw.data[0] == '1';
        } else {
            node.value.text = raw;
        }
    }
    call.input.arguments = {nodes_[index].data(), node_count};
    return pos == bytes;
}
ayni_status_t ModelTools::decode(ayni_bytes_v1 payload) {
    count_ = 0;
    if (!payload.data || payload.size < call_header_bytes || payload.size > wire_.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::memcpy(wire_.data(), payload.data, payload.size);
    const auto *data = wire_.data();
    uint32_t count = 1;
    if (u16(data)) {
        if (!decode_call(data, payload.size, 0)) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else {
        count = u16(data + node_count_offset);
        if (u16(data + name_length_offset) || u16(data + flags_offset) != batch_version || !count ||
            count > calls_.size()) {
            return AYNI_INVALID_ARGUMENT;
        }
        size_t pos = batch_header_bytes;
        for (uint32_t at = 0; at < count; ++at) {
            if (payload.size - pos < sizeof(uint32_t)) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto bytes = get32(data + pos);
            pos += sizeof(uint32_t);
            if (bytes > payload.size - pos || !decode_call(data + pos, bytes, at)) {
                return AYNI_INVALID_ARGUMENT;
            }
            pos += bytes;
        }
        if (pos != payload.size) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    // The shared codec owns UTF-8, argument-tree, duplicate-ID and storage-size rules.
    uint32_t encoded = 0;
    const auto rc = ayni_tool_calls_measure(calls_.data(), count, &encoded);
    if (rc != AYNI_OK) {
        return rc;
    }
    if (encoded > wire_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    count_ = count;
    return AYNI_OK;
}
} // namespace usbchat
