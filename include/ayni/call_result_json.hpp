// Original Call result projection. The saved Schema owns interpretation; current
// catalog definitions cannot reinterpret already executed outcomes or values.
#ifndef AYNI_CALL_RESULT_JSON_HPP
#define AYNI_CALL_RESULT_JSON_HPP
#include <ayni/calls.h>
#include <ayni/values_json.hpp>
namespace ayni::json {
// Leaves one object open for the caller to attach the chosen result body.
inline void write_call_facts(Writer &out, const ayni_task_call_result_v1 &result) {
    out.raw("{\"call_id\":");
    out.unsigned_number(result.logical_call_id);
    out.raw(",\"operation_id\":");
    out.unsigned_number(result.operation_id);
    out.raw(",\"outcome\":");
    out.unsigned_number(result.outcome);
    out.raw(",\"status\":");
    out.number(result.status);
    out.raw(",\"reason\":");
    out.unsigned_number(result.reason);
    out.raw(",\"effect\":");
    out.unsigned_number(result.effect);
}
inline ayni_status_t write_call_result(Writer &out, const ayni_task_call_result_v1 &result,
                                       uint64_t reference = 0) {
    write_call_facts(out, result);
    if (result.outcome == AYNI_CALL_COMPLETED || result.encoded_value.size || result.text.size) {
        struct State {
            const ayni_task_call_result_v1 &result;
            json::Writer &out;
        } inspection{result, out};
        const auto status = ayni_business_contract_read(
            result.original_domain, result.business_contract, &inspection,
            [](void *opaque, const ayni_business_contract_v1 *business) {
                auto &state = *static_cast<State *>(opaque);
                std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
                uint32_t count = 0;
                if (state.result.encoded_value.size) {
                    const auto decoded =
                        ayni_value_read(state.result.encoded_value, nodes.data(),
                                        static_cast<uint32_t>(nodes.size()), &count);
                    if (decoded != AYNI_OK) {
                        return decoded;
                    }
                } else {
                    count = 1;
                    nodes[0].kind = AYNI_VALUE_TEXT;
                    nodes[0].subtree_end = 1;
                    nodes[0].value.text = state.result.text;
                }
                state.out.raw(",\"result\":");
                return json::write_value(state.out, business->result, {nodes.data(), count});
            });
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (reference) {
        out.raw(",\"result_ref\":");
        out.unsigned_number(reference);
    }
    out.raw("}");
    return AYNI_OK;
}
// A summary changes only representation of the body; known action facts stay exact.
inline void write_call_summary(Writer &out, const ayni_task_call_result_v1 &result,
                               uint64_t reference, ayni_string_v1 summary) {
    write_call_facts(out, result);
    out.raw(",\"result_ref\":");
    out.unsigned_number(reference);
    out.raw(",\"body_kind\":\"unconfirmed_summary\",\"summary\":");
    out.string(summary);
    out.raw("}");
}
// Withheld content is independent from the retained operation's real outcome.
inline void write_call_unavailable(Writer &out, const ayni_task_call_result_v1 &result) {
    write_call_facts(out, result);
    out.raw(",\"body_kind\":\"unavailable\",\"body_status\":");
    out.number(AYNI_EXPIRED);
    out.raw(",\"message\":\"Known action must not be repeated.\"}");
}
} // namespace ayni::json
#endif
