#include <algorithm>
#include <ayni/runtime/operation_session.h>
#include <cstring>

namespace ayni::runtime {
namespace {
constexpr char operation_step[] = "operation";
ayni_string_v1 step_key() {
    return {operation_step, sizeof(operation_step) - 1U};
}
template <class T> T sized() {
    T result{};
    result.struct_size = sizeof(result);
    return result;
}
bool terminal_task(uint32_t state) {
    return state == AYNI_TASK_COMPLETED || state == AYNI_TASK_CANCELLED ||
           state == AYNI_TASK_FAILED || state == AYNI_TASK_EXPIRED;
}
bool terminal_goal(uint32_t state) {
    return state == AYNI_GOAL_SATISFIED || state == AYNI_GOAL_CANCELLED ||
           state == AYNI_GOAL_FAILED || state == AYNI_GOAL_EXPIRED;
}
} // namespace
ayni_status_t OperationSession::install(ayni_kernel_t *kernel, const Config &config) {
    if (installation_ != Installation::empty) {
        return AYNI_INVALID_STATE;
    }
    if (!kernel || !config.workspace || !config.requirements || !config.commands.submit ||
        !config.max_lifetime_us || !config.version || !config.binding_version) {
        return AYNI_INVALID_ARGUMENT;
    }
    installation_ = Installation::failed;
    config_ = config;
    auto strategy = sized<ayni_strategy_v1>();
    strategy.version = config.version;
    strategy.strategy_id = config.strategy_id;
    strategy.ctx = this;
    strategy.decide = decide;
    strategy.ready = ready;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    auto type = sized<ayni_goal_type_v1>();
    type.version = config.version;
    type.goal_type_id = config.goal_type;
    type.ctx = this;
    type.validate = validate;
    type.strategy_id = config.strategy_id;
    type.export_requirements = requirements;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
    status = ayni_register_goal_type(kernel, &type);
    if (status == AYNI_OK) {
        kernel_ = kernel;
        installation_ = Installation::ready;
    }
    return status;
}
ayni_status_t OperationSession::read(ayni_bytes_v1 bytes, ayni_value_v1 &out) {
    uint32_t count = 0;
    const auto status =
        ayni_value_read(config_.workspace, bytes, nodes_.data(), node_capacity, &count);
    if (status != AYNI_OK) {
        return status;
    }
    for (uint32_t at = 0; at < count; ++at) {
        if (nodes_[at].kind == AYNI_VALUE_BODY) {
            return AYNI_UNSUPPORTED;
        }
    }
    out = {nodes_.data(), count};
    return AYNI_OK;
}
ayni_status_t OperationSession::validate(void *context, ayni_bytes_v1 bytes) {
    auto &self = *static_cast<OperationSession *>(context);
    ayni_value_v1 value{};
    return bytes.size > input_capacity ? AYNI_CAPACITY_EXCEEDED : self.read(bytes, value);
}
ayni_status_t OperationSession::requirements(void *context, ayni_bytes_v1 bytes,
                                             const ayni_goal_requirement_sink_v1 *sink) {
    auto &self = *static_cast<OperationSession *>(context);
    return self.config_.requirements(self.config_.policy_context, bytes, sink);
}
uint32_t OperationSession::ready(void *context, uint64_t task) {
    const auto &state = static_cast<OperationSession *>(context)->state_;
    // Other management-created Goals of the same registered type never share this route's gate.
    return state.phase == Phase::active && task == state.task ? 1U : 0U;
}
ayni_status_t OperationSession::decide(void *context, const ayni_task_snapshot_v1 *task,
                                       ayni_policy_output_v1 *out) {
    auto &self = *static_cast<OperationSession *>(context);
    if (!task || !out || !task->call_result || !out->request_call) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (self.state_.phase != Phase::active || task->goal_id != self.state_.handle.goal ||
        task->now_us >= self.state_.expires_at_us) {
        out->disposition = AYNI_POLICY_OUTPUT_DEFER;
        return AYNI_OK;
    }
    auto result = sized<ayni_task_call_result_v1>();
    const auto queried = task->call_result(task->call_context, step_key(), &result);
    if (queried == AYNI_NOT_FOUND) {
        ayni_value_v1 value{};
        const auto read = self.read(task->goal_parameters, value);
        if (read != AYNI_OK) {
            return read;
        }
        auto request = sized<ayni_call_request_v1>();
        request.kind = AYNI_POLICY_REQUEST_CALL;
        request.binding_id = self.config_.binding_id;
        request.binding_version = self.config_.binding_version;
        request.step_key = step_key();
        request.value = value;
        return out->request_call(out->call_context, &request);
    }
    if (queried != AYNI_OK) {
        return queried;
    }
    if (!result.outcome) {
        return AYNI_BUSY;
    }
    // Retain the original terminal fact before Core clears task.logical_call_id.
    // This records a result, never authority or permission to retry it.
    if (result.outcome != AYNI_CALL_COMPLETED || result.status != AYNI_OK) {
        self.state_.status = result.outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN
                             : result.status == AYNI_OK            ? AYNI_DENIED
                                                                   : result.status;
        self.state_.reason = result.reason;
    }
    if (result.outcome == AYNI_CALL_ABANDONED || result.status == AYNI_RESULT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (!out->intents || !out->intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out->intents[0] = sized<ayni_policy_intent_v1>();
    out->intents[0].kind = result.outcome == AYNI_CALL_COMPLETED && result.status == AYNI_OK
                               ? AYNI_POLICY_REPORT_COMPLETION
                               : AYNI_POLICY_REPORT_FAILURE;
    out->intents[0].expected_revision = task->revision;
    out->intent_count = 1;
    return AYNI_OK;
}
ayni_status_t OperationSession::open(uint64_t boot, uint64_t owner, const ayni_value_v1 &input,
                                     uint64_t lifetime_us) {
    if (!kernel_) {
        return AYNI_NOT_READY;
    }
    if (state_.phase != Phase::idle) {
        return AYNI_BUSY;
    }
    if (!boot || !owner || !lifetime_us || lifetime_us > config_.max_lifetime_us) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto info = sized<ayni_kernel_info_v1>();
    const auto queried = ayni_get_kernel_info(kernel_, &info);
    if (queried != AYNI_OK) {
        return queried;
    }
    if (info.boot_epoch != boot) {
        return AYNI_STALE;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    if (generation_ == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t bytes = 0;
    auto status = ayni_value_write(&input, input_.data(), input_capacity, &bytes);
    if (status != AYNI_OK) {
        return status;
    }
    ayni_value_v1 checked{};
    status = read({input_.data(), bytes}, checked);
    if (status != AYNI_OK) {
        input_.fill(0);
        return status;
    }
    input_size_ = bytes;
    lifetime_us_ = lifetime_us;
    state_ = {Phase::creating, {boot, owner, 0, ++generation_}, 0, 0, AYNI_OK, AYNI_REASON_NONE};
    return AYNI_OK;
}
bool OperationSession::matches(OperationSessionHandle handle) const {
    const auto &current = state_.handle;
    return current.goal && handle.boot == current.boot && handle.owner == current.owner &&
           handle.goal == current.goal && handle.generation == current.generation;
}
ayni_status_t OperationSession::activate(OperationSessionHandle handle, uint64_t now_us) {
    if (!matches(handle)) {
        return AYNI_STALE;
    }
    if (state_.phase != Phase::ready_to_activate) {
        return AYNI_INVALID_STATE;
    }
    if (now_us >= state_.expires_at_us) {
        fail(AYNI_EXPIRED, AYNI_REASON_NONE);
        return AYNI_EXPIRED;
    }
    state_.phase = Phase::active;
    return AYNI_OK;
}
ayni_status_t OperationSession::close(OperationSessionHandle handle) {
    if (!matches(handle)) {
        return AYNI_STALE;
    }
    close_all();
    return AYNI_OK;
}
void OperationSession::close_all() {
    if (state_.phase == Phase::idle || state_.phase == Phase::terminal) {
        return;
    }
    state_.phase = state_.handle.goal ? Phase::closing : Phase::terminal;
}
void OperationSession::fail(ayni_status_t status, ayni_reason_t reason) {
    if (state_.status == AYNI_OK) {
        state_.status = status;
        state_.reason = reason;
    }
    close_all();
}
ayni_status_t OperationSession::observe(uint64_t durable_seq, uint64_t now_us) {
    auto goal = sized<ayni_goal_info_v1>();
    auto status = ayni_get_goal(kernel_, state_.handle.goal, &goal);
    if (status != AYNI_OK) {
        return status;
    } // Never silently forget an admitted Goal.
    if (goal.owner_instance_id != state_.handle.owner ||
        goal.work_profile.recovery != AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT) {
        return AYNI_DENIED;
    }
    state_.task = goal.last_task_id;
    state_.expires_at_us =
        state_.expires_at_us ? std::min(state_.expires_at_us, goal.expires_us) : goal.expires_us;
    if (now_us >= state_.expires_at_us && !terminal_goal(goal.lifecycle)) {
        fail(AYNI_EXPIRED, AYNI_REASON_NONE);
    }
    if (goal.lifecycle == AYNI_GOAL_PROPOSED && goal.commit_seq <= durable_seq) {
        fail(AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING);
    } else if (state_.phase == Phase::authorizing && goal.lifecycle == AYNI_GOAL_ACTIVE &&
               goal.commit_seq <= durable_seq) {
        state_.phase =
            Phase::ready_to_activate; // Strategy gate remains closed until handle delivery.
    }
    if (state_.phase == Phase::closing && !terminal_goal(goal.lifecycle)) {
        const ayni_goal_target_v1 target{sizeof(target), 0, goal.goal_id};
        const auto result = config_.commands.submit(
            config_.commands.context, AYNI_INTENT_GOAL_CANCEL, &target, nullptr, goal.revision);
        if (result.status != AYNI_OK && result.status != AYNI_BUSY) {
            return result.status;
        }
    }
    bool settled = terminal_goal(goal.lifecycle) && goal.commit_seq <= durable_seq;
    if (goal.last_task_id) {
        auto task = sized<ayni_task_info_v1>();
        status = ayni_get_task(kernel_, goal.last_task_id, &task);
        if (status != AYNI_OK) {
            return status;
        }
        settled = settled && terminal_task(task.lifecycle) && task.commit_seq <= durable_seq;
        if (task.lifecycle == AYNI_TASK_PAUSED && state_.phase == Phase::active) {
            fail(task.hold_status, task.hold_reason);
        }
        if (task.logical_call_id) {
            auto call = sized<ayni_call_info_v1>();
            status = ayni_get_call(kernel_, task.logical_call_id, &call);
            if (status != AYNI_OK) {
                return status;
            }
            settled = settled && call.outcome != 0;
            if (call.outcome && call.status != AYNI_OK) {
                state_.status = call.status;
                state_.reason = call.reason;
            }
        }
    }
    if (settled) {
        if (state_.status == AYNI_OK && goal.lifecycle != AYNI_GOAL_SATISFIED) {
            state_.status = goal.lifecycle == AYNI_GOAL_EXPIRED ? AYNI_EXPIRED : AYNI_DENIED;
        }
        state_.phase = Phase::terminal;
    }
    return AYNI_OK;
}
ayni_status_t OperationSession::advance(uint64_t durable_seq, uint64_t now_us) {
    if (!kernel_) {
        return AYNI_NOT_READY;
    }
    if (state_.phase == Phase::idle || state_.phase == Phase::terminal) {
        return AYNI_OK;
    }
    if (state_.phase == Phase::creating) {
        auto goal = sized<ayni_goal_create_v1>();
        goal.goal_type_id = config_.goal_type;
        goal.goal_type_version = config_.version;
        goal.owner_instance_id = state_.handle.owner;
        goal.trigger = AYNI_TRIGGER_ONCE;
        goal.parameters = {input_.data(), input_size_};
        goal.expiry_us = lifetime_us_;
        const auto created = config_.commands.submit(
            config_.commands.context, AYNI_INTENT_GOAL_CREATE, &goal, &state_.handle.goal, 0);
        if (created.status == AYNI_BUSY) {
            return AYNI_OK;
        }
        if (created.status != AYNI_OK) {
            fail(created.status, created.reason);
            return AYNI_OK;
        }
        if (!state_.handle.goal) {
            // An OK create must return the admitted identity. Never repeat an ambiguous create.
            fail(AYNI_INTERNAL, AYNI_REASON_NONE);
            return AYNI_INTERNAL;
        }
        state_.phase = Phase::authorizing;
    }
    return observe(durable_seq, now_us);
}
ayni_status_t OperationSession::release() {
    if (state_.phase != Phase::terminal) {
        return AYNI_BUSY;
    }
    input_.fill(0);
    nodes_.fill({});
    input_size_ = 0;
    lifetime_us_ = 0;
    state_ = {};
    return AYNI_OK;
}
bool OperationSession::owns(ayni_string_v1 type, uint32_t version) const {
    return version == config_.version && type.size == config_.goal_type.size && type.data &&
           !std::memcmp(type.data, config_.goal_type.data, type.size);
}
} // namespace ayni::runtime
