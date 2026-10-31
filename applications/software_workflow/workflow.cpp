// Portable sample application: Task-owned sum and event stream (ADR-0086/0087).
#include <ayni/applications/software_workflow.h>
#include <ayni/ayni.hpp>
#include <ayni/detail/workspace.hpp>
#include <ayni/directory.h>
#include <ayni/operations.h>
#include <cstdio>
#include <cstring>
#include <iterator>
namespace ayni::applications::software_workflow {
namespace {
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
ayni_scope_v1 empty_scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
bool equal(ayni_string_v1 value, const char *expected) {
    return value.size == std::strlen(expected) &&
           std::memcmp(value.data, expected, value.size) == 0;
}
ayni_status_t report(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &out) {
    if (!out.intents || !out.intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out.intents[0] = initialized<ayni_policy_intent_v1>();
    out.intents[0].kind = AYNI_POLICY_REPORT_COMPLETION;
    out.intents[0].expected_revision = snapshot.revision;
    out.intent_count = 1;
    return AYNI_OK;
}
struct Example {
    static constexpr uint32_t checkpoint_bytes = 1;
    static constexpr uint8_t observed = 1;
    static constexpr int64_t items = 512;
    static ayni_status_t requirements(void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        for (const auto id : {"software.sum", "software.events"}) {
            const ayni_goal_requirement_v1 requirement{sizeof(requirement), 1, text(id),
                                                       empty_scope()};
            const auto status = sink->emit(sink->ctx, &requirement);
            if (status != AYNI_OK) {
                return status;
            }
        }
        return AYNI_OK;
    }
    static ayni_status_t start(ayni_policy_output_v1 &out, const char *key, const char *binding) {
        auto call = ayni::initialized<ayni_call_request_v1>();
        call.kind = AYNI_POLICY_REQUEST_CALL;
        call.step_key = text(key);
        call.binding_id = text(binding);
        call.binding_version = 1;
        call.mode = AYNI_CALL_MODE_MANAGED;
        ayni::ValueBuilder<1> input;
        if (equal(call.binding_id, "software.sum")) {
            input.integer(0, items);
        } else {
            input.text(0, {});
        }
        const auto status = input.finish(call.value);
        return status == AYNI_OK ? out.request_call(out.call_context, &call) : status;
    }
    // C-SDK-006: the same Task-scoped directory exposes extension-owned explanations.
    static ayni_status_t inspect_tools(const ayni_task_snapshot_v1 &snapshot) {
        for (const auto *id : {"software.sum", "software.events"}) {
            auto detail = initialized<ayni_capability_detail_v1>();
            const auto inspected =
                snapshot.inspect_capability(snapshot.call_context, text(id), 1, &detail);
            if (inspected != AYNI_OK) {
                return inspected;
            }
            if (!detail.usage.purpose.size || !detail.business_contract.size) {
                return AYNI_INVALID_STATE;
            }
        }
        return AYNI_OK;
    }
    static bool correct_sum(const ayni_workspace_v1 *workspace,
                            const ayni_task_call_result_v1 &job) {
        ayni_value_node_v1 result{};
        uint32_t count = 0;
        return ayni_value_read(workspace, job.encoded_value, &result, 1, &count) == AYNI_OK &&
               count == 1 && result.kind == AYNI_VALUE_I64 &&
               result.value.integer == items * (items + 1) / 2;
    }
    static ayni_status_t decide(void *context, const ayni_task_snapshot_v1 *snapshot,
                                ayni_policy_output_v1 *out) {
        auto job = ayni::initialized<ayni_task_call_result_v1>();
        auto stream = ayni::initialized<ayni_task_call_result_v1>();
        auto status = snapshot->call_result(snapshot->call_context, text("job"), &job);
        if (status == AYNI_NOT_FOUND) {
            const auto inspected = inspect_tools(*snapshot);
            if (inspected != AYNI_OK) {
                return inspected;
            }
            return start(*out, "job", "software.sum");
        }
        if (status != AYNI_OK) {
            return status;
        }
        status = snapshot->call_result(snapshot->call_context, text("stream"), &stream);
        if (status == AYNI_NOT_FOUND) {
            return start(*out, "stream", "software.events");
        }
        if (status != AYNI_OK) {
            return status;
        }
        const auto &target = job.outcome ? stream : job;
        const ayni_operation_ref_v1 reference{snapshot->task_id, target.logical_call_id,
                                              target.operation_id};
        auto info = ayni::initialized<ayni_operation_info_v1>();
        status = snapshot->operation(snapshot->call_context, &reference, &info);
        if (status != AYNI_OK) {
            return status;
        }
        if (job.outcome && job.outcome != AYNI_CALL_COMPLETED) {
            return AYNI_INVALID_STATE;
        }
        if (job.outcome && !correct_sum(static_cast<const ayni_workspace_v1 *>(context), job)) {
            return AYNI_INVALID_STATE;
        }
        if (stream.outcome) {
            return snapshot->checkpoint.state.size == checkpoint_bytes &&
                           snapshot->checkpoint.state.data[0] == observed &&
                           stream.outcome == AYNI_CALL_CANCELLED
                       ? report(*snapshot, *out)
                       : AYNI_INVALID_STATE;
        }
        char key[AYNI_CATALOG_ID_MAX_BYTES];
        const auto size = std::snprintf(key, sizeof(key), "control.%u",
                                        static_cast<unsigned>(snapshot->revision));
        if (size <= 0 || static_cast<size_t>(size) >= sizeof(key)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto control = ayni::initialized<ayni_operation_control_v1>();
        control.step_key = {key, static_cast<uint32_t>(size)};
        control.reference = reference;
        if (job.outcome && info.sample.usable) {
            if (out->checkpoint_capacity < checkpoint_bytes) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            // Observation evidence belongs to this Task, never a shared application instance.
            out->checkpoint[0] = observed;
            out->checkpoint_size = checkpoint_bytes;
            control.kind = AYNI_OPERATION_CLOSE;
        } else {
            control.kind = AYNI_OPERATION_WAIT;
            control.after = info.frontier;
            control.timeout_ms = AYNI_OPERATION_WAIT_MS_MAX;
        }
        return out->control_operation(out->call_context, &control);
    }
};
} // namespace
ayni_status_t configure(void *context, ayni_kernel_t *kernel) {
    if (!kernel || !context) {
        return AYNI_INVALID_ARGUMENT;
    }
    size_t bytes = 0, alignment = 0;
    const auto query = ayni_value_workspace_requirements(&bytes, &alignment);
    if (query != AYNI_OK) {
        return query;
    }
    const auto checked =
        ::ayni::detail::Workspace::check(static_cast<const ayni_workspace_v1 *>(context), bytes);
    if (checked != AYNI_OK) {
        return checked;
    }
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.version = 1;
    strategy.strategy_id = text("example.operations");
    strategy.ctx = context;
    strategy.decide = Example::decide;
    strategy.max_intents_per_call = 1;
    strategy.max_checkpoint_bytes = Example::checkpoint_bytes;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    auto type = initialized<ayni_goal_type_v1>();
    type.version = 1;
    type.goal_type_id = text("example.managed");
    type.strategy_id = strategy.strategy_id;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.export_requirements = Example::requirements;
    const ayni_tool_ref_v1 tools[] = {{text("software.sum"), 1, AYNI_TOOL_ROLE_CAPABILITY},
                                      {text("software.events"), 1, AYNI_TOOL_ROLE_CAPABILITY}};
    type.tool_count = static_cast<uint32_t>(std::size(tools));
    type.tools = tools;
    return ayni_register_goal_type(kernel, &type);
}
} // namespace ayni::applications::software_workflow
