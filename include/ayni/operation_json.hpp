// Model-neutral operation token and response codec; no allocation, I/O or authority decisions.
#ifndef AYNI_OPERATION_JSON_HPP
#define AYNI_OPERATION_JSON_HPP
#include <ayni/operations.h>
#include <ayni/values_json.hpp>
#include <charconv>
#include <cstring>
#include <string_view>
namespace ayni::operations {
inline constexpr uint32_t wide_digits = 20; // Maximum decimal uint64 width.
inline constexpr uint32_t reference_fields = 3;
inline constexpr uint32_t frontier_fields = 7;
inline constexpr uint32_t token_prefix_bytes = 3; // "o1:" or "f1:".
inline constexpr uint32_t reference_bytes =
    token_prefix_bytes + reference_fields * (wide_digits + 1);
inline constexpr uint32_t frontier_bytes = token_prefix_bytes + frontier_fields * (wide_digits + 1);
// One exact decimal token codec; leading zeros and trailing fields are noncanonical.
template <size_t N>
inline bool decode(ayni_string_v1 input, std::string_view prefix, uint64_t (&fields)[N]) {
    if ((!input.data && input.size) || input.size > token_prefix_bytes + N * (wide_digits + 1)) {
        return false;
    }
    std::string_view source(input.data ? input.data : "", input.size);
    if (source.substr(0, prefix.size()) != prefix) {
        return false;
    }
    source.remove_prefix(prefix.size());
    for (size_t at = 0; at < N; ++at) {
        const auto end = at + 1 == N ? source.size() : source.find(':');
        if (end == std::string_view::npos || !end || (end > 1 && source.front() == '0')) {
            return false;
        }
        const auto number = std::from_chars(source.data(), source.data() + end, fields[at]);
        if (number.ec != std::errc{} || number.ptr != source.data() + end) {
            return false;
        }
        source.remove_prefix(end);
        if (at + 1 < N) {
            source.remove_prefix(1);
        }
    }
    return source.empty();
}
// Writer shares canonical field order with the decoder; buffer failure publishes no token.
template <size_t N>
inline bool encode(std::string_view prefix, const uint64_t (&fields)[N], char *buffer,
                   uint32_t capacity, ayni_string_v1 &out) {
    if (!buffer || capacity < prefix.size()) {
        return false;
    }
    uint32_t used = static_cast<uint32_t>(prefix.size());
    std::memcpy(buffer, prefix.data(), used);
    for (size_t at = 0; at < N; ++at) {
        if (at) {
            if (used == capacity) {
                return false;
            }
            buffer[used++] = ':';
        }
        const auto written = std::to_chars(buffer + used, buffer + capacity, fields[at]);
        if (written.ec != std::errc{}) {
            return false;
        }
        used = static_cast<uint32_t>(written.ptr - buffer);
    }
    out = {buffer, used};
    return true;
}
inline bool reference(ayni_string_v1 source, ayni_operation_ref_v1 &out) {
    enum Field { task, call, operation, count };
    uint64_t values[count]{};
    if (!decode(source, "o1:", values)) {
        return false;
    }
    out = {values[task], values[call], values[operation]};
    return true;
}
inline bool reference(const ayni_operation_ref_v1 &ref, char *buffer, ayni_string_v1 &out) {
    const uint64_t fields[]{ref.task_id, ref.logical_call_id, ref.operation_id};
    return encode("o1:", fields, buffer, reference_bytes, out);
}
inline bool frontier(ayni_string_v1 source, ayni_operation_frontier_v1 &out) {
    enum Field { boot, sequence, gaps, phase, cancel, response, outcome, count };
    uint64_t values[count]{};
    if (!decode(source, "f1:", values)) {
        return false;
    }
    for (size_t at = phase; at < count; ++at) {
        if (values[at] > UINT32_MAX) {
            return false;
        }
    }
    out = {values[boot],
           values[sequence],
           values[gaps],
           static_cast<uint32_t>(values[phase]),
           static_cast<uint32_t>(values[cancel]),
           static_cast<uint32_t>(values[response]),
           static_cast<uint32_t>(values[outcome])};
    return true;
}
inline bool frontier(const ayni_operation_frontier_v1 &value, char *buffer, ayni_string_v1 &out) {
    const uint64_t fields[]{value.boot_epoch,       value.sequence,      value.gaps,   value.phase,
                            value.cancel_requested, value.cancel_result, value.outcome};
    return encode("f1:", fields, buffer, frontier_bytes, out);
}
// Wide identities/times are always decimal strings, including beyond JSON's exact integer range.
inline void unsigned_string(json::Writer &out, uint64_t value) {
    out.raw("\"");
    out.unsigned_number(value);
    out.raw("\"");
}
// Formats actual copied facts; pending, cancellation and effect remain independent.
inline void write(json::Writer &out, const ayni_operation_info_v1 &info,
                  ayni_status_t wait_status = AYNI_OK) {
    char ref_buffer[reference_bytes], after_buffer[frontier_bytes];
    ayni_string_v1 ref{}, after{};
    if (!reference(info.reference, ref_buffer, ref) ||
        !frontier(info.frontier, after_buffer, after)) {
        out.raw("null");
        return;
    }
    out.raw("{\"operation_ref\":");
    out.string(ref);
    out.raw(",\"after\":");
    out.string(after);
    out.raw(",\"form\":");
    out.number(info.form);
    out.raw(",\"phase\":");
    out.number(info.frontier.phase);
    out.raw(",\"outcome\":");
    out.number(info.call.outcome);
    out.raw(",\"effect\":");
    out.number(info.call.effect);
    out.raw(",\"status\":");
    out.number(info.call.status);
    out.raw(",\"reason\":");
    out.number(info.call.reason);
    out.raw(",\"wait_status\":");
    out.number(wait_status);
    out.raw(",\"cancel_requested\":");
    out.raw(info.call.cancel_requested ? "true" : "false");
    out.raw(",\"cancel_result\":");
    out.number(info.call.cancel_result);
    out.raw(",\"sample\":");
    if (info.sample.struct_size && info.sample.usable) {
        out.raw("{\"usable\":true,\"value\":");
        if (info.sample.kind == AYNI_OBSERVATION_BOOL) {
            out.raw(info.sample.value ? "true" : "false");
        } else {
            out.raw("\"");
            out.number(info.sample.value);
            out.raw("\"");
        }
        out.raw(",\"kind\":");
        out.number(info.sample.kind);
        out.raw(",\"origin\":");
        out.number(info.sample.origin);
        out.raw(",\"quality\":");
        out.number(info.sample.quality);
        out.raw(",\"unit\":");
        out.string(ayni_string_v1{info.sample.unit.data, info.sample.unit.size});
        out.raw(",\"captured_at_us\":");
        unsigned_string(out, info.sample.captured_at_us);
        out.raw(",\"valid_until_us\":");
        unsigned_string(out, info.sample.valid_until_us);
        out.raw(",\"gaps\":");
        unsigned_string(out, info.sample.sequence_gaps);
        const auto &source = info.sample.reference;
        out.raw(",\"source_id\":");
        out.string(ayni_string_v1{source.source_id.data, source.source_id.size});
        out.raw(",\"source_version\":");
        out.unsigned_number(source.source_version);
        out.raw(",\"boot_epoch\":");
        unsigned_string(out, source.boot_epoch);
        out.raw(",\"generation\":");
        unsigned_string(out, source.generation);
        out.raw(",\"sequence\":");
        unsigned_string(out, source.sequence);
        out.raw(",\"subscription_id\":");
        unsigned_string(out, source.subscription_id);
        out.raw("}");
    } else {
        out.raw("null");
    }
    out.raw("}");
}
} // namespace ayni::operations
#endif
