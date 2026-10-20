// C ABI entry points of the work model (ADR-0017): registration of
// principals, agents, goal types and strategies, sessions, intents and their
// queries.
// They check arguments and struct sizes, then whether a callback is running,
// then the lifecycle state (contracts/domain/lifecycles.md); the rules of
// sessions and intents live in kernel/intent/. Every struct here is new in
// 0.5 or later or changed since, so its only layout is its sizeof.

#include "kernel/abi_checks.h"
#include "kernel/intent/intent.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_state.h"

#include <ayni/ayni.h>

#include <cstring>

namespace {

using ayni::kernel::check_struct_size;

template <typename Struct> ayni_status_t check_size(const Struct &value) {
    return check_struct_size(value.struct_size, sizeof(Struct), sizeof(Struct));
}

// Queries answer from RECOVERING to DRAINING, with state that may not be
// durable yet (C-COMMIT-004).
bool answers_queries(ayni_lifecycle_t lifecycle) {
    return lifecycle != AYNI_LIFECYCLE_CONFIGURING && lifecycle != AYNI_LIFECYCLE_STOPPED;
}

bool is_channel(ayni_channel_t channel) {
    return channel == AYNI_CHANNEL_VOICE || channel == AYNI_CHANNEL_MANAGEMENT;
}

// Registering an id twice is an argument error; the capacity comes last.
template <typename AddResult> ayni_status_t add_status(AddResult result) {
    switch (result) {
    case AddResult::added:
        return AYNI_OK;
    case AddResult::duplicate:
        return AYNI_INVALID_ARGUMENT;
    case AddResult::full:
        break;
    }
    return AYNI_CAPACITY_EXCEEDED;
}

// Runs steps 1 to 5 of ayni_submit_intent once out_error is known to be
// writable or absent; failure receives the detail of a refusal.
ayni_status_t submit(ayni_kernel_t *kernel, const ayni_intent_v1 *intent,
                     ayni_intent_result_v1 *out_result, ayni::kernel::IntentFailure &failure) {
    if (kernel == nullptr || intent == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (out_result != nullptr) {
        const ayni_status_t status = check_size(*out_result);
        if (status != AYNI_OK) {
            return status;
        }
    }
    ayni::kernel::Intent decoded{};
    ayni_status_t status = ayni::kernel::decode_intent(*intent, decoded, failure);
    if (status != AYNI_OK) {
        return status;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    if (kernel->lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    ayni::kernel::IntentOutcome outcome{};
    const uint64_t now_us = kernel->host.monotonic_now_us(kernel->host.ctx);
    status = ayni::kernel::apply_intent(kernel->work, decoded, now_us, outcome, failure);
    if (status == AYNI_OK && out_result != nullptr) {
        ayni_intent_result_v1 result{};
        result.struct_size = sizeof(result);
        result.object = outcome.object;
        result.commit_seq = outcome.commit_seq;
        *out_result = result;
    }
    if (status == AYNI_OK && outcome.due_work) {
        kernel->host.wake(kernel->host.ctx);
    }
    return status;
}

void write_goal_info(const ayni_kernel &kernel, uint32_t index, ayni_goal_info_v1 &out) {
    const ayni::work::Goal &goal = kernel.work.goals.at(index);
    const ayni::catalog::GoalType &type = kernel.work.goal_types.at(goal.goal_type_index);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = goal.lifecycle;
    info.goal_id = goal.goal_id;
    info.parent_goal_id = goal.parent_goal_id;
    info.owner_instance_id = goal.owner_instance_id;
    info.origin_principal_id = goal.origin.principal_id;
    info.transition_id = goal.created_transition_id;
    info.commit_seq = goal.commit_seq;
    info.revision = goal.revision;
    info.cancel_revision = goal.cancel_revision;
    info.origin = goal.origin.kind;
    info.origin_channel = goal.origin.channel;
    info.origin_binding_revision = goal.origin.binding_revision;
    info.depth = goal.depth;
    info.goal_type_version = type.version;
    info.goal_type_id_size = type.id.size();
    std::memcpy(info.goal_type_id, type.id.data(), type.id.size());
    info.trigger = goal.trigger.kind;
    info.occurrences = goal.cursor.taken;
    info.last_task_id = goal.last_task_id;
    info.trigger_delay_us = goal.trigger.delay_us;
    info.trigger_period_us = goal.trigger.period_us;
    info.next_due_us = goal.cursor.next_due_us;
    info.expires_us = goal.expires_us;
    info.occurrences_skipped = goal.occurrences_skipped;
    info.skip_reason = goal.skip_reason;
    info.trigger_source_size = goal.trigger_source.size();
    std::memcpy(info.trigger_source, goal.trigger_source.data(), goal.trigger_source.size());
    // Read from the tables rather than kept, so a query needs no clock.
    if (goal.cursor.has_next()) {
        info.wait_reason =
            ayni::work::derivation_blocker(kernel.work.tasks, index, kernel.work.ids.available());
    }
    out = info;
}

void write_task_info(const ayni::work::Task &task, ayni_task_info_v1 &out) {
    ayni_task_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = task.lifecycle;
    info.task_id = task.task_id;
    info.goal_id = task.goal_id;
    info.owner_instance_id = task.owner_instance_id;
    info.transition_id = task.created_transition_id;
    info.commit_seq = task.commit_seq;
    info.revision = task.revision;
    info.cancel_revision = task.cancel_revision;
    info.occurrence = task.occurrence;
    info.occurrence_count = task.occurrence_count;
    info.due_us = task.due_us;
    info.hold_status = task.hold_status;
    info.hold_reason = task.hold_reason;
    out = info;
}

} // namespace

extern "C" {

ayni_status_t ayni_register_principal(ayni_kernel_t *kernel,
                                      const ayni_principal_desc_v1 *principal) {
    if (kernel == nullptr || principal == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*principal);
    if (status != AYNI_OK) {
        return status;
    }
    if (principal->kind != AYNI_PRINCIPAL_OWNER || principal->principal_id == 0U ||
        principal->binding_revision == 0U || principal->reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->work.principals.add_owner(
               {principal->principal_id, principal->binding_revision, principal->kind})
               ? AYNI_OK
               : AYNI_CAPACITY_EXCEEDED;
}

ayni_status_t ayni_register_agent(ayni_kernel_t *kernel, const ayni_agent_desc_v1 *agent) {
    if (kernel == nullptr || agent == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*agent);
    if (status != AYNI_OK) {
        return status;
    }
    if (agent->agent_id == 0U || agent->reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return add_status(kernel->work.agents.add(agent->agent_id));
}

ayni_status_t ayni_register_goal_type(ayni_kernel_t *kernel, const ayni_goal_type_v1 *goal_type) {
    if (kernel == nullptr || goal_type == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*goal_type);
    if (status != AYNI_OK) {
        return status;
    }
    using ayni::catalog::GoalTypeRegistry;
    ayni::catalog::GoalType type{};
    // Goals that may only have no trigger have no tasks to decide.
    const bool has_tasks = goal_type->trigger_kinds != AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE);
    if (goal_type->version == 0U || !type.id.assign(goal_type->goal_type_id) ||
        goal_type->trigger_kinds == 0U ||
        (goal_type->trigger_kinds & ~GoalTypeRegistry::defined_trigger_kinds) != 0U ||
        goal_type->reserved != 0U ||
        (has_tasks ? !type.strategy_id.assign(goal_type->strategy_id)
                   : goal_type->strategy_id.size != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    type.version = goal_type->version;
    type.ctx = goal_type->ctx;
    type.validate = goal_type->validate;
    type.trigger_kinds = goal_type->trigger_kinds;
    type.occurrence_wait_max_us = goal_type->occurrence_wait_max_us;
    return add_status(kernel->work.goal_types.add(type));
}

ayni_status_t ayni_register_strategy(ayni_kernel_t *kernel, const ayni_strategy_v1 *strategy) {
    if (kernel == nullptr || strategy == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_status_t status = check_size(*strategy);
    if (status != AYNI_OK) {
        return status;
    }
    // The kernel lays out the intents at its own stride (DecisionBuffer).
    status = check_struct_size(strategy->intent_size, sizeof(ayni_policy_intent_v1),
                               sizeof(ayni_policy_intent_v1));
    if (status != AYNI_OK) {
        return status;
    }
    ayni::catalog::Strategy entry{};
    if (strategy->version == 0U || !entry.id.assign(strategy->strategy_id) ||
        strategy->decide == nullptr || strategy->max_intents_per_call == 0U ||
        strategy->max_intents_per_call > kernel->decisions.capacity()) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    entry.version = strategy->version;
    entry.ctx = strategy->ctx;
    entry.decide = strategy->decide;
    entry.max_intents_per_call = strategy->max_intents_per_call;
    return add_status(kernel->work.strategies.add(entry));
}

ayni_status_t ayni_open_session(ayni_kernel_t *kernel, const ayni_session_desc_v1 *session,
                                uint64_t *out_session_id) {
    if (kernel == nullptr || session == nullptr || out_session_id == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*session);
    if (status != AYNI_OK) {
        return status;
    }
    if (!is_channel(session->channel)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    if (kernel->lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    return ayni::kernel::open_session(kernel->work, session->principal_id, session->channel,
                                      *out_session_id);
}

ayni_status_t ayni_close_session(ayni_kernel_t *kernel, uint64_t session_id) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->in_callback() || !answers_queries(kernel->lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    return kernel->work.sessions.close(session_id) ? AYNI_OK : AYNI_NOT_FOUND;
}

ayni_status_t ayni_submit_intent(ayni_kernel_t *kernel, const ayni_intent_v1 *intent,
                                 ayni_intent_result_v1 *out_result, ayni_error_v1 *out_error) {
    if (out_error != nullptr) {
        const ayni_status_t status = check_size(*out_error);
        if (status != AYNI_OK) {
            return status;
        }
    }
    ayni::kernel::IntentFailure failure{};
    const ayni_status_t status = submit(kernel, intent, out_result, failure);
    if (out_error != nullptr) {
        ayni_error_v1 error{};
        error.struct_size = sizeof(error);
        error.status = status;
        error.reason = failure.reason;
        error.object = failure.object;
        *out_error = error;
    }
    return status;
}

ayni_status_t ayni_get_agent(const ayni_kernel_t *kernel, uint64_t agent_id,
                             ayni_agent_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    if (!answers_queries(kernel->lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    const ayni::work::AgentInstance *agent = kernel->work.agents.find_agent(agent_id);
    if (agent == nullptr) {
        return AYNI_NOT_FOUND;
    }
    ayni_agent_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = agent->lifecycle;
    info.agent_id = agent->agent_id;
    info.instance_id = agent->instance_id;
    info.commit_seq = agent->stamp.commit_seq;
    info.revision = agent->revision;
    *out_info = info;
    return AYNI_OK;
}

ayni_status_t ayni_get_goal(const ayni_kernel_t *kernel, uint64_t goal_id,
                            ayni_goal_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    if (!answers_queries(kernel->lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->work.goals.find(goal_id, index)) {
        return AYNI_NOT_FOUND;
    }
    write_goal_info(*kernel, index, *out_info);
    return AYNI_OK;
}

ayni_status_t ayni_get_task(const ayni_kernel_t *kernel, uint64_t task_id,
                            ayni_task_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    if (!answers_queries(kernel->lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    uint32_t index = 0;
    if (!kernel->work.tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    write_task_info(kernel->work.tasks.at(index), *out_info);
    return AYNI_OK;
}

} // extern "C"
