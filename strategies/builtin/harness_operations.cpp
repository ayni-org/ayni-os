// Fixed-function operation controls reuse the public Task-scoped owner boundary.
#include "harness.h"
#include <ayni/call_result_json.hpp>
namespace ayni::strategies::detail {
namespace {
enum Field {
    operation_field,
    reference_field,
    after_field,
    timeout_field,
    binding_field,
    version_field,
    arguments_field
};
bool read_arguments(const ayni_tool_call_v1 &call, NativeArguments &args) {
    return args.read(call, {"operation", "operation_ref", "after", "timeout_ms", "binding_id",
                            "binding_version", "arguments"});
}
bool name(ayni_string_v1 value, std::string_view expected) {
    return equal(value, view(expected));
}
ayni_status_t target(const NativeArguments &args, const ayni_task_snapshot_v1 &snapshot,
                     ayni_operation_ref_v1 &reference, ayni_operation_info_v1 &info) {
    ayni_string_v1 encoded{};
    if (!args.string(reference_field, encoded) || !operations::reference(encoded, reference)) {
        return AYNI_INVALID_ARGUMENT;
    }
    info.struct_size = sizeof(info);
    return snapshot.operation ? snapshot.operation(snapshot.call_context, &reference, &info)
                              : AYNI_UNSUPPORTED;
}
// Format only true metadata plus an original known terminal body, preserving outcome/effect.
ayni_status_t render(Workspace &workspace, const ayni_operation_info_v1 &info,
                     const ayni_task_call_result_v1 *call, ayni_status_t wait_status, char *buffer,
                     uint32_t capacity, ayni_string_v1 &response) {
    json::Writer json(buffer, capacity);
    json.raw("{\"operation\":");
    operations::write(json, info, wait_status);
    json.raw(",\"result\":");
    if (call) {
        const auto status =
            ::ayni::detail::with_workspace(workspace, json::call_result_workspace_requirements,
                                           [&](const ayni_workspace_v1 *region) {
                                               return json::write_call_result(region, json, *call);
                                           });
        if (status != AYNI_OK) {
            return status;
        }
    } else {
        json.raw("null");
    }
    json.raw("}");
    size_t size = 0;
    if (!json.finish(size)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    response = {buffer, static_cast<uint32_t>(size)};
    return AYNI_OK;
}
} // namespace

bool step_committed(const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key) {
    for (uint32_t at = 0; at < snapshot.step_count; ++at) {
        if (equal(snapshot.steps[at].step_key, key)) {
            return true;
        }
    }
    return false;
}

ayni_status_t operation_source(const ayni_tool_call_v1 &proposal,
                               const ayni_task_snapshot_v1 &snapshot,
                               ayni_task_call_result_v1 &call) {
    NativeArguments args;
    ayni_operation_ref_v1 reference{};
    ayni_operation_info_v1 info{};
    if (!read_arguments(proposal, args)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = target(args, snapshot, reference, info);
    return status == AYNI_OK ? result(snapshot, {info.step_key.data, info.step_key.size}, call)
                             : status;
}

// Result summaries retain the original shared body/proof graph; no separate backend archive.
ayni_status_t operation_result(Workspace &workspace, const ayni_operation_info_v1 &info,
                               const ToolLoopConfig &config, const ayni_task_snapshot_v1 &snapshot,
                               Turn &turn, ayni_policy_output_v1 &output, char *buffer,
                               uint32_t capacity, ToolBudget budget, ayni_string_v1 &response,
                               ayni_string_v1 protocol_id, ayni_status_t wait_status) {
    ayni_task_call_result_v1 original{};
    const ayni_task_call_result_v1 *body = nullptr;
    std::string owned;
    if (info.call.outcome && info.call.outcome != AYNI_CALL_ABANDONED &&
        info.call.effect != AYNI_CALL_EFFECT_UNKNOWN) {
        const auto read = result(snapshot, {info.step_key.data, info.step_key.size}, original);
        if (read != AYNI_OK && read != AYNI_EXPIRED) {
            return read;
        }
        if (read == AYNI_OK) {
            const auto resolved = resolve_result_body(*config.bodies, original, owned);
            if (resolved == AYNI_NOT_READY) {
                output.disposition = AYNI_POLICY_OUTPUT_DEFER;
            }
            if (resolved != AYNI_OK) {
                return resolved;
            }
            body = &original;
        }
    }
    const auto status = render(workspace, info, body, wait_status, buffer, capacity, response);
    if (status == AYNI_OK && budget.fits(response)) {
        return body ? retain_tool_result(config, turn, protocol_id, *body, response, output)
                    : AYNI_OK;
    }
    if (body && (status == AYNI_OK || status == AYNI_CAPACITY_EXCEEDED)) {
        response = {};
        return begin_result_summary(config, turn, *body, output);
    }
    return status == AYNI_OK ? AYNI_CAPACITY_EXCEEDED : status;
}

// Controls stage one intent; a committed control only reads back, so retries cannot
// reopen/re-cancel.
ayni_status_t native_operation(Workspace &workspace, const ayni_tool_call_v1 &proposal,
                               const ToolLoopConfig &config, const ayni_task_snapshot_v1 &snapshot,
                               Turn &turn, ayni_string_v1 step, ayni_policy_output_v1 &output,
                               char *buffer, uint32_t capacity, ToolBudget budget,
                               ayni_string_v1 &response, bool &requested, bool &handled) {
    handled = false;
    if (!name(proposal.function_name, model::call_function)) {
        return AYNI_OK;
    }
    NativeArguments args;
    ayni_string_v1 operation{};
    if (!read_arguments(proposal, args)) {
        handled = true;
        return native_error(AYNI_INVALID_ARGUMENT, "Invalid operation envelope.", buffer, capacity,
                            response);
    }
    if (args.positions[operation_field] == UINT32_MAX) {
        return AYNI_OK;
    }
    if (args.string(operation_field, operation) &&
        (name(operation, "start") || name(operation, "invoke"))) {
        return AYNI_OK;
    }
    handled = true;
    const bool wait = name(operation, "wait");
    const bool read = name(operation, "read");
    const bool cancel = name(operation, "cancel");
    const bool close = name(operation, "close");
    ayni_operation_ref_v1 reference{};
    ayni_operation_info_v1 info{};
    ayni_status_t status = (wait || read || cancel || close) && args.count == (wait ? 4U : 2U)
                               ? target(args, snapshot, reference, info)
                               : AYNI_INVALID_ARGUMENT;
    if (status != AYNI_OK) {
        return native_error(status, "Operation unavailable or invalid arguments.", buffer, capacity,
                            response);
    }
    ayni_operation_control_v1 control{};
    control.struct_size = sizeof(control);
    control.kind = wait     ? AYNI_OPERATION_WAIT
                   : cancel ? AYNI_OPERATION_CANCEL
                            : AYNI_OPERATION_CLOSE;
    control.step_key = step;
    control.reference = reference;
    if (wait) {
        ayni_string_v1 after{};
        if (!args.string(after_field, after) || !operations::frontier(after, control.after) ||
            !args.integer(timeout_field, control.timeout_ms)) {
            return native_error(AYNI_INVALID_ARGUMENT, "Invalid wait frontier or duration.", buffer,
                                capacity, response);
        }
    }
    if (!read && !step_committed(snapshot, step)) {
        status = output.control_operation ? output.control_operation(output.call_context, &control)
                                          : AYNI_UNSUPPORTED;
        if (status == AYNI_OK) {
            requested = true;
            return AYNI_OK;
        }
        if (status != AYNI_STALE || reference.operation_id != info.reference.operation_id) {
            return native_error(status, "Control refused; execution facts are unchanged.", buffer,
                                capacity, response);
        }
    }
    const auto waited =
        wait && !info.call.outcome && ayni_operation_frontier_equal(&info.frontier, &control.after)
            ? AYNI_EXPIRED
            : AYNI_OK;
    return operation_result(workspace, info, config, snapshot, turn, output, buffer, capacity,
                            budget, response, proposal.protocol_id, waited);
}
} // namespace ayni::strategies::detail
