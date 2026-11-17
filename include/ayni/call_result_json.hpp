// Original Call result projection. The saved Schema owns interpretation; current
// catalog definitions cannot reinterpret already executed outcomes or values.
#ifndef AYNI_CALL_RESULT_JSON_HPP
#define AYNI_CALL_RESULT_JSON_HPP
#include <array>
#include <ayni/calls.h>
#include <ayni/detail/workspace.hpp>
#include <ayni/values_json.hpp>
namespace ayni::json {
// One proof and one nested value decode coexist with the output node array.
// Since 0.53; query before starting a worker or invoking a strategy.
inline ayni_status_t call_result_workspace_requirements(size_t *bytes, size_t *alignment) {
    if (!bytes || !alignment) {
        return AYNI_INVALID_ARGUMENT;
    }
    size_t proof = 0, value = 0, align = 0;
    auto status = ayni_business_workspace_requirements(&proof, &align);
    if (status != AYNI_OK) {
        return status;
    }
    status = ayni_value_workspace_requirements(&value, &align);
    if (status != AYNI_OK) {
        return status;
    }
    constexpr size_t alignment_slots = 3;
    *bytes = proof + value + sizeof(ayni_value_node_v1) * AYNI_VALUE_NODE_COUNT_MAX +
             alignment_slots * ::ayni::detail::Workspace::alignment;
    *alignment = align;
    return AYNI_OK;
}
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
namespace detail {
struct CallResultInspection {
    const ayni_task_call_result_v1 &result; // Borrowed immutable Call fact through receive.
    json::Writer &out; // Borrowed destination; failures retain Writer's existing semantics.
    ::ayni::detail::Workspace &workspace; // Remaining exclusive lanes beyond the live proof.
};
// The original result Schema interprets the body while the proof lane remains live.
inline ayni_status_t write_original_result(void *opaque,
                                           const ayni_business_contract_v1 *business) {
    auto &state = *static_cast<CallResultInspection *>(opaque);
    auto nodes_lease =
        state.workspace.make<std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX>>();
    if (!nodes_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &nodes = *nodes_lease;
    uint32_t count = 0;
    if (state.result.encoded_value.size) {
        size_t value_bytes = 0, value_alignment = 0;
        const auto value_query = ayni_value_workspace_requirements(&value_bytes, &value_alignment);
        if (value_query != AYNI_OK) {
            return value_query;
        }
        // C-SDK-007: ayni_value_read owns and erases its scratch before returning.
        auto value = state.workspace.delegate(value_bytes);
        if (!value) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const auto decoded = ayni_value_read(value.get(), state.result.encoded_value, nodes.data(),
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
}
} // namespace detail
inline ayni_status_t write_call_result(const ayni_workspace_v1 *region, Writer &out,
                                       const ayni_task_call_result_v1 &result,
                                       uint64_t reference = 0) {
    size_t bytes = 0, alignment = 0;
    const auto query = call_result_workspace_requirements(&bytes, &alignment);
    if (query != AYNI_OK) {
        return query;
    }
    const auto checked = ::ayni::detail::Workspace::check(region, bytes);
    if (checked != AYNI_OK) {
        return checked;
    }
    ::ayni::detail::Workspace workspace(region->data, region->byte_capacity);
    size_t proof_bytes = 0;
    const auto proof_query = ayni_business_workspace_requirements(&proof_bytes, &alignment);
    if (proof_query != AYNI_OK) {
        return proof_query;
    }
    // C-SDK-007: the proof codec erases its own leases on success and refusal.
    auto proof = workspace.delegate(proof_bytes);
    if (!proof) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    write_call_facts(out, result);
    if (result.outcome == AYNI_CALL_COMPLETED || result.encoded_value.size || result.text.size) {
        detail::CallResultInspection inspection{result, out, workspace};
        const auto status = ayni_business_contract_read(proof.get(), result.original_domain,
                                                        result.business_contract, &inspection,
                                                        detail::write_original_result);
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
