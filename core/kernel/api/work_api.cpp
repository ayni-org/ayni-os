#include "kernel/service/memory/memory_access.h"
// C ABI entry points of the work model (ADR-0017): registration of
// principals, agents, goal types and strategies, sessions, intents and their
// queries.
// They check arguments and struct sizes, then their gate (api_gate.h): whether
// a callback is running and the lifecycle state; the rules of
// sessions and intents live in kernel/intent/. Every struct here is new in
// 0.5 or later or changed since, so its only layout is its sizeof.

#include "catalog/abi_checks.h"
#include "catalog/capability/field_domain.h"
#include "kernel/api/api_gate.h"
#include "kernel/runtime/kernel_state.h"
#include "kernel/service/intent/authority_intent.h"
#include "kernel/service/intent/intent.h"
#include "kernel/service/model/work_model.h"

#include <ayni/authority.h>
#include <ayni/ayni.h>
#include <ayni/host_authority.h>

#include <cstring>

namespace {

using ayni::catalog::check_struct_size;

template <typename Struct> ayni_status_t check_size(const Struct &value) {
    return check_struct_size(value.struct_size, sizeof(Struct), sizeof(Struct));
}

bool is_channel(ayni_channel_t channel) {
    return channel == AYNI_CHANNEL_VOICE || channel == AYNI_CHANNEL_MANAGEMENT;
}

// Copies a goal type's tool allow list (ADR-0054). Bindings resolve at start,
// so only the references' shape and distinct binding ids are checked here.
bool read_tools(const ayni_goal_type_v1 &input, ayni::catalog::GoalType &type) {
    if (input.tool_count > AYNI_GOAL_TYPE_TOOL_COUNT_MAX || input.tool_reserved != 0U ||
        (input.tool_count != 0U && input.tools == nullptr)) {
        return false;
    }
    for (uint32_t at = 0; at < input.tool_count; ++at) {
        const ayni_tool_ref_v1 &tool = input.tools[at];
        if (tool.binding_version == 0U || tool.reserved != 0U ||
            !type.tools[at].binding_id.assign(tool.binding_id)) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (type.tools[before].binding_id.equals(tool.binding_id)) {
                return false;
            }
        }
        type.tools[at].binding_version = tool.binding_version;
    }
    type.tool_count = input.tool_count;
    return true;
}

// ADR-0056: reject ambiguous/invalid declarations before copying a registration.
bool read_checkpoint_readers(const ayni_strategy_v1 &input, ayni::catalog::Strategy &entry) {
    if (input.reader_count > AYNI_CHECKPOINT_READER_COUNT_MAX || input.reader_reserved != 0U ||
        (input.reader_count != 0U && input.readers == nullptr)) {
        return false;
    }
    for (uint32_t at = 0; at < input.reader_count; ++at) {
        const auto &reader = input.readers[at];
        if (!entry.readers[at].id.assign(reader.strategy_id) || reader.min_version == 0U ||
            reader.max_version < reader.min_version) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (entry.readers[before].id.equals(reader.strategy_id)) {
                return false;
            }
        }
        entry.readers[at].min_version = reader.min_version;
        entry.readers[at].max_version = reader.max_version;
    }
    entry.reader_count = input.reader_count;
    return true;
}

// Runs steps 1 to 5 of ayni_submit_intent once out_error is known to be
// writable or absent; failure receives the detail of a refusal.
// C-WIRE-004/C-AUTH-027: decode and apply before publishing a result; an exact
// command always reports its retained receipt, including retries and pending
// revokes. Commandless work reports only its fresh queued transition.
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    ayni::kernel::IntentOutcome outcome{};
    const bool log_was_idle = kernel->writer_idle();
    bool committed = false;
    status = kernel->submit_intent(decoded, outcome, failure, committed);
    if (status == AYNI_OK && out_result != nullptr) {
        ayni_intent_result_v1 result{};
        result.struct_size = sizeof(result);
        if (decoded.command_id != 0U) {
            const auto *session = kernel->model().sessions.find(decoded.session_id);
            ayni_intent_receipt_v1 receipt{};
            if (session == nullptr ||
                kernel->intent_receipt(session->principal_id, decoded.command_id, receipt) !=
                    AYNI_OK) {
                return AYNI_INTERNAL;
            }
            result.object = receipt.object;
            result.commit_seq = receipt.commit_seq;
            result.phase = receipt.phase;
            result.effect = receipt.effect;
            result.applied_boot_epoch = receipt.applied_boot_epoch;
        } else {
            // Without a command identity this is a fresh transition. Submit
            // only queues its batch; the writer appends in a later step.
            result.object = outcome.object;
            result.commit_seq = outcome.commit_seq;
            result.effect = outcome.effect;
            result.phase = AYNI_INTENT_APPLIED;
            result.applied_boot_epoch = kernel->writer_boot_epoch();
        }
        *out_result = result;
    }
    // The owner appends the batch, and learns the next deadline, in its next
    // step. A batch already waiting woke it, and the post of an append's
    // result wakes it again.
    if ((committed && log_was_idle) || (status == AYNI_OK && outcome.due_work)) {
        kernel->wake();
    }
    return status;
}

void write_goal_info(const ayni_kernel &kernel, uint32_t index, ayni_goal_info_v1 &out) {
    const ayni::work::Goal &goal = kernel.model().goals.at(index);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = goal.lifecycle;
    info.goal_id = goal.goal_id;
    info.parent_goal_id = goal.parent_goal_id;
    info.owner_instance_id = goal.owner_instance_id;
    info.origin_principal_id = goal.origin.principal_id;
    info.transition_id = goal.created.transition_id;
    info.commit_seq = goal.commit_seq;
    info.revision = goal.revision;
    info.cancel_revision = goal.cancel_revision;
    info.origin = goal.origin.kind;
    info.origin_channel = goal.origin.channel;
    info.origin_binding_revision = goal.origin.binding_revision;
    info.depth = goal.depth;
    info.goal_type_version = goal.goal_type_version;
    info.goal_type_id_size = goal.goal_type_id.size();
    std::memcpy(info.goal_type_id, goal.goal_type_id.data(), goal.goal_type_id.size());
    info.trigger = goal.trigger.kind;
    info.occurrences = goal.cursor.taken;
    info.last_task_id = goal.last_task_id;
    info.trigger_delay_us = goal.trigger.delay_us;
    info.trigger_period_us = goal.trigger.period_us;
    info.next_due_us = goal.cursor.next_due_us;
    info.expires_us = goal.expires_us;
    info.original_expiry_us = goal.expiry_after_us;
    info.work_profile = goal.work_profile.snapshot();
    info.occurrences_skipped = goal.occurrences_skipped;
    info.skip_reason = goal.skip_reason;
    info.hold_reason = goal.hold_reason;
    if (ayni::work::GoalTable::is_live(goal.lifecycle) && goal.lifecycle != AYNI_GOAL_PROPOSED &&
        info.hold_reason == AYNI_REASON_NONE) {
        const auto authority = kernel.model().check_goal_authority(index, kernel.now_us(),
                                                                   kernel.writer_durable_seq());
        if (authority.status != AYNI_OK) {
            info.lifecycle = AYNI_GOAL_PAUSED;
            info.hold_reason = authority.reason;
        }
    }
    info.trigger_source_size = goal.trigger_source.size();
    std::memcpy(info.trigger_source, goal.trigger_source.data(), goal.trigger_source.size());
    // The capacity wait reason is derived from tables without a clock read.
    if (goal.cursor.has_next()) {
        info.wait_reason = ayni::work::derivation_blocker(kernel.model().tasks, index,
                                                          kernel.model().ids.available());
    }
    out = info;
}

// Queries observe the immediate authority gate before bounded cleanup runs;
// retained Task progress and existing non-authority holds remain unchanged.
void write_task_info(const ayni_kernel &kernel, uint32_t index, ayni_task_info_v1 &out) {
    const auto &task = kernel.model().tasks.at(index);
    ayni_task_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = ayni::work::TaskTable::effective_lifecycle(task);
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
    info.hold_status = ayni::work::TaskTable::effective_hold_status(task);
    info.hold_reason = ayni::work::TaskTable::effective_hold_reason(task);
    info.step_count = task.step_count;
    info.model_calls_used = task.model_calls.used;
    info.model_call_limit = task.model_calls.limit;
    info.model_call_hard_limit = kernel.model().tasks.model_call_hard_limit();
    info.model_call_limit_reached = task.model_calls.blocked ? 1U : 0U;
    info.logical_call_id = task.call.id;
    if (ayni::work::TaskTable::is_live(task.lifecycle) && task.lifecycle != AYNI_TASK_CANCELLING &&
        info.hold_status == AYNI_OK) {
        const auto authority = kernel.model().check_task_authority(index, kernel.now_us(),
                                                                   kernel.writer_durable_seq());
        if (authority.status != AYNI_OK) {
            info.lifecycle = AYNI_TASK_PAUSED;
            info.hold_status = authority.status;
            info.hold_reason = authority.reason;
        }
    }
    out = info;
}

// Copy one canonical field for Grant and Goal queries. Both persisted region
// kinds share DomainReader; failures publish no partial output.
ayni_status_t copy_authority_field(ayni_bytes_v1 bytes, uint32_t field_index,
                                   ayni_grant_field_info_v1 &out_field) {
    ayni::catalog::DomainReader reader(bytes);
    ayni::catalog::DomainField field{};
    if (field_index >= reader.field_count()) {
        return AYNI_NOT_FOUND;
    }
    for (uint32_t at = 0; at <= field_index; ++at) {
        if (!reader.next(field)) {
            return AYNI_INTERNAL;
        }
    }
    ayni_grant_field_info_v1 info{};
    info.struct_size = sizeof(info);
    info.field_id = field.id;
    info.kind = field.values.kind;
    info.usage = field.usage;
    info.unit_size = field.values.unit.size;
    if (info.unit_size != 0U) {
        std::memcpy(info.unit, field.values.unit.data, info.unit_size);
    }
    if (info.kind == AYNI_AUTHORITY_I64) {
        info.values.integer = {field.values.minimum, field.values.maximum};
    } else {
        auto &tokens = info.values.tokens;
        tokens.member_count = field.values.token_count;
        for (uint32_t at = 0; at < tokens.member_count; ++at) {
            tokens.member_sizes[at] = field.values.tokens[at].size;
            std::memcpy(tokens.members[at], field.values.tokens[at].data, tokens.member_sizes[at]);
        }
    }
    out_field = info;
    return AYNI_OK;
}

// Query framing before borrowing a requirement; all callers preserve outputs.
ayni_status_t find_requirement(const ayni_kernel *kernel, uint64_t goal_id, uint32_t at,
                               ayni::work::GoalRequirement &out) {
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    ayni::work::GoalAuthorityRecord record{};
    if (!kernel->model().goals.find(goal_id, index) ||
        !kernel->model().goal_authority.read(index, goal_id, record) || at >= record.count) {
        return AYNI_NOT_FOUND;
    }
    out = record.requirements[at];
    return AYNI_OK;
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(ayni::authority::Principal{
        principal->principal_id, principal->binding_revision, principal->kind});
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure_agent(agent->agent_id);
}

ayni_status_t ayni_register_capability(ayni_kernel_t *kernel,
                                       const ayni_capability_desc_v1 *capability) {
    if (kernel == nullptr || capability == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni::catalog::DomainEncoding encoding{};
    const ayni_status_t status = ayni::catalog::CapabilityRegistry::validate(*capability, encoding);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->configure(*capability, encoding);
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
        !ayni::catalog::WorkProfile::decode(goal_type->work_profile, type.work_profile) ||
        !type.work_profile.accepts_occurrence_wait(goal_type->occurrence_wait_max_us) ||
        (type.work_profile.kind == AYNI_WORK_BACKGROUND &&
         goal_type->trigger_kinds != AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL)) ||
        (has_tasks ? !type.strategy_id.assign(goal_type->strategy_id)
                   : goal_type->strategy_id.size != 0U) ||
        !read_tools(*goal_type, type)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    type.version = goal_type->version;
    type.ctx = goal_type->ctx;
    type.validate = goal_type->validate;
    type.export_requirements = goal_type->export_requirements;
    type.trigger_kinds = goal_type->trigger_kinds;
    type.occurrence_wait_max_us = goal_type->occurrence_wait_max_us;
    return kernel->configure(type);
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
        strategy->max_intents_per_call > kernel->decision_capacity() ||
        strategy->max_checkpoint_bytes > kernel->model().tasks.checkpoint_max_bytes() ||
        strategy->reserved != 0U || !read_checkpoint_readers(*strategy, entry)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::registration);
        gated != AYNI_OK) {
        return gated;
    }
    entry.version = strategy->version;
    entry.ctx = strategy->ctx;
    entry.decide = strategy->decide;
    entry.max_intents_per_call = strategy->max_intents_per_call;
    entry.max_checkpoint_bytes = strategy->max_checkpoint_bytes;
    return kernel->configure(entry);
}

// C-AUTH-010/016: resolve the registered principal once, then require the
// current durable binding before allocating a fresh session identity.
ayni_status_t ayni_open_session(ayni_kernel_t *kernel, const ayni_session_desc_v1 *session,
                                uint64_t *out_session_id) {
    if (kernel == nullptr || session == nullptr || out_session_id == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_status_t status = check_size(*session);
    if (status != AYNI_OK) {
        return status;
    }
    if (!is_channel(session->channel) || session->binding_revision == 0U ||
        session->reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::running);
        gated != AYNI_OK) {
        return gated;
    }
    const auto *principal = kernel->model().principals.find(session->principal_id);
    if (principal == nullptr) {
        return AYNI_NOT_FOUND;
    }
    if (principal->binding_revision != session->binding_revision) {
        return AYNI_DENIED;
    }
    const auto *binding = kernel->model().principals.binding();
    if (binding == nullptr || binding->stamp.commit_seq > kernel->durable_seq()) {
        return AYNI_NOT_READY;
    }
    return kernel->open_session(*principal, session->channel, *out_session_id);
}

ayni_status_t ayni_close_session(ayni_kernel_t *kernel, uint64_t session_id) {
    if (kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::session_close);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->close_session(session_id);
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

// The output prefix is checked before lookup. A failed or unavailable receipt
// leaves the caller's complete object unchanged (C-AUTH-027).
ayni_status_t ayni_get_intent_receipt(const ayni_kernel_t *kernel, uint64_t principal_id,
                                      uint64_t command_id, ayni_intent_receipt_v1 *out_receipt) {
    if (kernel == nullptr || out_receipt == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_receipt);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    return kernel->intent_receipt(principal_id, command_id, *out_receipt);
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    const ayni::work::AgentInstance *agent = kernel->model().agents.find_agent(agent_id);
    if (agent == nullptr) {
        return AYNI_NOT_FOUND;
    }
    ayni_agent_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = ayni::work::AgentTable::effective_lifecycle(*agent);
    info.agent_id = agent->agent_id;
    info.instance_id = agent->instance_id;
    info.commit_seq = agent->stamp.commit_seq;
    info.revision = agent->revision;
    info.fault_reason = agent->fault_reason;
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().goals.find(goal_id, index)) {
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
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    write_task_info(*kernel, index, *out_info);
    return AYNI_OK;
}

// Export only the completed durable decision, so an outer conversation store
// cannot discard old rounds based on an uncommitted summary (ADR-0056).
ayni_status_t ayni_copy_task_checkpoint(const ayni_kernel_t *kernel, uint64_t task_id,
                                        uint8_t *buffer, uint32_t capacity, uint32_t *out_size) {
    if (kernel == nullptr || out_size == nullptr || (capacity != 0U && buffer == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &task = kernel->model().tasks.at(index);
    if (task.lifecycle != AYNI_TASK_COMPLETED) {
        return ayni::work::TaskTable::is_live(task.lifecycle) ? AYNI_BUSY : AYNI_INVALID_STATE;
    }
    if (task.commit_seq > kernel->durable_seq()) {
        return AYNI_BUSY;
    }
    if (!ayni::kernel::task_content_current(
            {kernel->model(), kernel->now_us(), kernel->durable_seq()}, index)) {
        return AYNI_EXPIRED;
    }
    const auto checkpoint = kernel->model().tasks.checkpoint(index);
    if (capacity < checkpoint.size) {
        *out_size = checkpoint.size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (checkpoint.size != 0U) {
        std::memcpy(buffer, checkpoint.data, checkpoint.size);
    }
    *out_size = checkpoint.size;
    return AYNI_OK;
}

// Retained facts and their effective usable-chain status are distinct views.
// C-AUTH-020/024: query reports both without reviving or changing the Grant.
ayni_status_t ayni_get_grant(const ayni_kernel_t *kernel, uint64_t grant_id,
                             ayni_grant_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().grants.find(grant_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &grant = kernel->model().grants.at(index);
    ayni_grant_info_v1 info{};
    info.struct_size = sizeof(info);
    info.lifecycle = grant.lifecycle;
    info.grant_id = grant.grant_id;
    info.subject_instance_id = grant.subject_instance_id;
    info.parent_grant_id = grant.parent_grant_id;
    info.owner_principal_id = grant.owner_principal_id;
    info.owner_binding_revision = grant.owner_binding_revision;
    info.capability_version = grant.capability_version;
    info.capability_id_size = grant.capability_id.size();
    std::memcpy(info.capability_id, grant.capability_id.data(), info.capability_id_size);
    info.field_count =
        ayni::catalog::DomainReader(kernel->model().grants.scope(index)).field_count();
    info.allow_delegation = grant.allow_delegation ? 1U : 0U;
    info.depth = grant.depth;
    info.lifetime = grant.lifetime;
    info.revision = grant.revision;
    info.duration_us = grant.duration_us;
    info.deadline_us = grant.deadline_us;
    info.created_transition_id = grant.created_stamp.transition_id;
    info.created_commit_seq = grant.created_stamp.commit_seq;
    info.transition_id = grant.stamp.transition_id;
    info.commit_seq = grant.stamp.commit_seq;
    info.revoked_by_root_id = grant.revoked_by_root_id;
    info.cleanup_after_grant_id = grant.cleanup_after_grant_id;
    info.cleanup_through_grant_id = grant.cleanup_through_grant_id;
    info.cleanup_transition_id = grant.cleanup_stamp.transition_id;
    info.cleanup_commit_seq = grant.cleanup_stamp.commit_seq;
    info.bound_goal_id = grant.bound_goal_id;
    info.effective_reason =
        kernel->model()
            .check_grant(index, kernel->now_us(), kernel->durable_seq(), grant.bound_goal_id)
            .reason;
    *out_info = info;
    return AYNI_OK;
}

ayni_status_t ayni_get_grant_field(const ayni_kernel_t *kernel, uint64_t grant_id,
                                   uint32_t field_index, ayni_grant_field_info_v1 *out_field) {
    if (kernel == nullptr || out_field == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_field);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().grants.find(grant_id, index)) {
        return AYNI_NOT_FOUND;
    }
    return copy_authority_field(kernel->model().grants.scope(index), field_index, *out_field);
}

// Copy immutable input bytes; sizing cannot expose a partially copied result.
ayni_status_t ayni_get_goal_parameters(const ayni_kernel_t *kernel, uint64_t goal_id,
                                       uint8_t *buffer, uint32_t capacity, uint32_t *out_size) {
    if (kernel == nullptr || out_size == nullptr || (buffer == nullptr && capacity != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().goals.find(goal_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto parameters = kernel->model().goals.parameters(index);
    if (capacity < parameters.size) {
        *out_size = parameters.size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (parameters.size != 0U) {
        std::memcpy(buffer, parameters.data, parameters.size);
    }
    *out_size = parameters.size;
    return AYNI_OK;
}

// Summary and per-entry queries expose saved evidence separately from current
// usability. A query cannot select new Grants or clear an authorization pause.
ayni_status_t ayni_get_goal_authority(const ayni_kernel_t *kernel, uint64_t goal_id,
                                      ayni_goal_authority_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().goals.find(goal_id, index)) {
        return AYNI_NOT_FOUND;
    }
    ayni_goal_authority_info_v1 info{};
    info.struct_size = sizeof(info);
    info.goal_id = goal_id;
    const uint64_t now = kernel->now_us();
    info.effective_reason =
        kernel->model().check_goal_authority(index, now, kernel->writer_durable_seq()).reason;
    ayni::work::GoalAuthorityRecord record{};
    if (kernel->model().goal_authority.read(index, goal_id, record)) {
        info.phase = static_cast<ayni_goal_authority_phase_t>(record.phase);
        info.transition_id = record.stamp.transition_id;
        info.commit_seq = record.stamp.commit_seq;
        info.requirement_count = record.count;
        info.binding_commit_seq = record.binding.established_seq;
        info.parent_binding_commit_seq = record.binding.parent.commit_seq;
        for (uint32_t at = 0; at < record.count; ++at) {
            if (kernel->model()
                    .check_requirement(record.requirements[at], now, kernel->writer_durable_seq(),
                                       goal_id)
                    .status != AYNI_OK) {
                ++info.missing_count;
            }
        }
    }
    *out_info = info;
    return AYNI_OK;
}

// Return the immutable ceiling identity and its original selected Grant.
// Current availability is computed separately; this query never reselects.
ayni_status_t ayni_get_goal_requirement(const ayni_kernel_t *kernel, uint64_t goal_id,
                                        uint32_t index, ayni_goal_requirement_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    ayni::work::GoalRequirement requirement{};
    const auto found = find_requirement(kernel, goal_id, index, requirement);
    if (found != AYNI_OK) {
        return found;
    }
    ayni_goal_requirement_info_v1 info{};
    info.struct_size = sizeof(info);
    info.capability_version = requirement.capability_version;
    info.grant_id = requirement.dependency.grant_id;
    info.grant_revision = requirement.dependency.revision;
    info.capability_id_size = requirement.capability_id.size();
    std::memcpy(info.capability_id, requirement.capability_id.data(), info.capability_id_size);
    info.field_count = ayni::catalog::DomainReader(requirement.scope).field_count();
    info.effective_reason =
        kernel->model()
            .check_requirement(requirement, kernel->now_us(), kernel->writer_durable_seq(), goal_id)
            .reason;
    *out_info = info;
    return AYNI_OK;
}

// Copy original domain and normalized Scope together. Keeping both on a local
// output preserves the caller's complete buffer on either field-reader failure.
ayni_status_t ayni_get_goal_requirement_field(const ayni_kernel_t *kernel, uint64_t goal_id,
                                              uint32_t requirement_index, uint32_t field_index,
                                              ayni_goal_requirement_field_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = check_size(*out_info);
    if (status != AYNI_OK) {
        return status;
    }
    ayni::work::GoalRequirement requirement{};
    const auto found = find_requirement(kernel, goal_id, requirement_index, requirement);
    if (found != AYNI_OK) {
        return found;
    }
    ayni_goal_requirement_field_info_v1 info{};
    info.struct_size = sizeof(info);
    const auto domain = copy_authority_field(requirement.source_domain, field_index, info.domain);
    if (domain != AYNI_OK) {
        return domain;
    }
    const auto scope = copy_authority_field(requirement.scope, field_index, info.scope);
    if (scope != AYNI_OK) {
        return scope;
    }
    *out_info = info;
    return AYNI_OK;
}

// Task metadata is a separate admission snapshot; the immutable ceiling is
// still owned by its Goal. Publish one complete caller-owned output on success.
ayni_status_t ayni_get_task_authority(const ayni_kernel_t *kernel, uint64_t task_id,
                                      ayni_task_authority_info_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto size = check_size(*out_info);
    if (size != AYNI_OK) {
        return size;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &task = kernel->model().tasks.at(index);
    ayni_task_authority_info_v1 info{};
    info.struct_size = sizeof(info);
    info.task_id = task_id;
    info.goal_id = task.goal_id;
    info.effective_reason =
        kernel->model()
            .check_task_authority(index, kernel->now_us(), kernel->writer_durable_seq())
            .reason;
    ayni::work::TaskAuthorityRecord record{};
    if (kernel->model().task_authority.read(task.slot, task_id, record)) {
        info.phase = static_cast<ayni_task_authority_phase_t>(record.phase);
        info.binding_commit_seq = record.ceiling.commit_seq;
        info.transition_id = record.stamp.transition_id;
        info.commit_seq = record.stamp.commit_seq;
        info.requirement_count = record.count;
    }
    *out_info = info;
    return AYNI_OK;
}

// Copy one retained selection after all fallible checks, preserving the
// caller's entire output on failure and exposing no borrowed storage.
ayni_status_t ayni_get_task_authority_reference(const ayni_kernel_t *kernel, uint64_t task_id,
                                                uint32_t at,
                                                ayni_task_authority_reference_v1 *out_info) {
    if (kernel == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto size = check_size(*out_info);
    if (size != AYNI_OK) {
        return size;
    }
    if (const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
        gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!kernel->model().tasks.find(task_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &task = kernel->model().tasks.at(index);
    ayni::work::TaskAuthorityRecord record{};
    if (!kernel->model().task_authority.read(task.slot, task_id, record) || at >= record.count) {
        return AYNI_NOT_FOUND;
    }
    const auto reference = record.references[at];
    const ayni_task_authority_reference_v1 info{sizeof(info), reference.revision,
                                                reference.grant_id, at, 0};
    *out_info = info;
    return AYNI_OK;
}

} // extern "C"
