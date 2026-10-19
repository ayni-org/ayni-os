// C ABI entry points of the work model (ADR-0017): registration of
// principals, agents and goal types, sessions, intents and their queries.
// They check arguments and struct sizes before the lifecycle state
// (contracts/domain/lifecycles.md); the intent rules live in
// kernel/intent/. Every struct here is new in 0.5, so its only layout is its
// sizeof.

#include "kernel/abi_checks.h"
#include "kernel/intent/intent.h"
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
    ayni::kernel::IntentOutcome outcome{};
    status = kernel->submit_intent(decoded, outcome, failure);
    if (status == AYNI_OK && out_result != nullptr) {
        ayni_intent_result_v1 result{};
        result.struct_size = sizeof(result);
        result.object = outcome.object;
        result.commit_seq = outcome.commit_seq;
        *out_result = result;
    }
    return status;
}

void write_goal_info(const ayni_kernel &kernel, uint32_t index, ayni_goal_info_v1 &out) {
    const ayni::work::Goal &goal = kernel.goals.at(index);
    const ayni::catalog::GoalType &type = kernel.goal_types.at(goal.goal_type_index);
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
    if (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return kernel->principals.add_owner(
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
    if (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    return add_status(kernel->agents.add(agent->agent_id));
}

ayni_status_t ayni_register_goal_type(ayni_kernel_t *kernel, const ayni_goal_type_v1 *goal_type) {
    if (kernel == nullptr || goal_type == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*goal_type);
    if (status != AYNI_OK) {
        return status;
    }
    ayni::catalog::GoalType type{};
    if (goal_type->version == 0U || !type.id.assign(goal_type->goal_type_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kernel->lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    type.version = goal_type->version;
    type.ctx = goal_type->ctx;
    type.validate = goal_type->validate;
    return add_status(kernel->goal_types.add(type));
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
    if (kernel->lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    if (kernel->principals.find(session->principal_id) == nullptr) {
        return AYNI_NOT_FOUND;
    }
    uint64_t session_id = 0;
    if (kernel->sessions.full() || !kernel->ids.allocate(session_id)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    // Cannot fail: the table has room.
    static_cast<void>(kernel->sessions.open({session_id, session->principal_id, session->channel}));
    *out_session_id = session_id;
    return AYNI_OK;
}

ayni_status_t ayni_close_session(ayni_kernel_t *kernel, uint64_t session_id) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!answers_queries(kernel->lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    return kernel->sessions.close(session_id) ? AYNI_OK : AYNI_NOT_FOUND;
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
    const ayni::work::AgentInstance *agent = kernel->agents.find_agent(agent_id);
    if (agent == nullptr) {
        return AYNI_NOT_FOUND;
    }
    ayni_agent_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = agent->lifecycle;
    info.agent_id = agent->agent_id;
    info.instance_id = agent->instance_id;
    info.commit_seq = agent->stamp.commit_seq;
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
    if (!kernel->goals.find(goal_id, index)) {
        return AYNI_NOT_FOUND;
    }
    write_goal_info(*kernel, index, *out_info);
    return AYNI_OK;
}

} // extern "C"
