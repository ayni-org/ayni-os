#include "kernel/service/intent/intent.h"

#include "catalog/abi_checks.h"
#include "catalog/catalog_id.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "foundation/digest.h"
#include "kernel/service/command/command_approval.h"
#include "kernel/service/command/command_canonical.h"
#include "kernel/service/command/intent_traits.h"
#include <ayni/calls.h>

namespace ayni::kernel {

using catalog::check_struct_size;
namespace {

// Each body struct is new in 0.5 or later or changed since, so each has one
// layout.
template <typename Body> ayni_status_t read_body(const void *body, const Body *&out_body) {
    out_body = static_cast<const Body *>(body);
    return check_struct_size(out_body->struct_size, sizeof(Body), sizeof(Body));
}

// A target intent names the goal, task, instance or Grant it changes.
template <typename Body>
ayni_status_t read_target(const void *body, uint64_t Body::*id, uint64_t &out_id) {
    const Body *target = nullptr;
    const ayni_status_t status = read_body(body, target);
    if (status != AYNI_OK) {
        return status;
    }
    if (target->reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    out_id = target->*id;
    return AYNI_OK;
}

// Both public requests use exactly one decoder for immutable Goal creation.
ayni_status_t read_goal_create(const ayni_goal_create_v1 &body, GoalCreate &out) {
    const auto status = check_struct_size(body.struct_size, sizeof(body), sizeof(body));
    if (status != AYNI_OK) {
        return status;
    }
    if (body.reserved != 0U || body.pause_group_reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = {body.goal_type_id,      body.goal_type_version,
           body.owner_instance_id, body.parent_goal_id,
           body.parameters,        {body.trigger, body.trigger_delay_us, body.trigger_period_us},
           body.expiry_us,         body.trigger_source};
    out.pause_group = body.pause_group;
    if (body.calendar != nullptr) {
        const auto decoded = work::decode_calendar_rule(*body.calendar, out.calendar);
        if (decoded != AYNI_OK) {
            return decoded;
        }
    }
    return AYNI_OK;
}

ayni_status_t read_scene_change(const ayni_intent_v1 &raw, authority::SceneChange &out) {
    const ayni_scene_set_enabled_v1 *body = nullptr;
    const auto status = read_body(raw.body, body);
    if (status != AYNI_OK) {
        return status;
    }
    if (body->reserved != 0 || body->enabled > 1 || !out.scene.assign(body->scene_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out.definition_version = body->definition_version;
    out.expected_revision = raw.expected_revision;
    out.mode = body->enabled != 0 ? authority::SceneMode::enabled : authority::SceneMode::disabled;
    return AYNI_OK;
}

// Decode the complete owner control before command lookup or any state change.
ayni_status_t read_pause_group_change(const ayni_intent_v1 &raw, work::PauseGroupChange &out) {
    const ayni_pause_group_set_v1 *body = nullptr;
    const auto status = read_body(raw.body, body);
    if (status != AYNI_OK) {
        return status;
    }
    if (body->reserved != 0 || body->paused > 1) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = {body->group, raw.expected_revision, body->paused != 0};
    return AYNI_OK;
}

// Decode one complete limit request without introducing work-state validation.
ayni_status_t read_task_limit(const ayni_intent_v1 &raw, Intent &out_intent) {
    const ayni_task_raise_model_limit_v1 *body = nullptr;
    const auto status = read_body(raw.body, body);
    if (status == AYNI_OK) {
        out_intent.target_id = body->task_id;
        out_intent.model_call_limit = body->model_call_limit;
    }
    return status;
}

ayni_status_t decode_body(const ayni_intent_v1 &raw, Intent &out_intent) {
    switch (raw.kind) {
    case AYNI_INTENT_CONTEXT_OPEN:
    case AYNI_INTENT_CONTEXT_PUBLISH:
    case AYNI_INTENT_CONTEXT_FORGET:
    case AYNI_INTENT_CONTEXT_ACK:
        return decode_context_command(raw, out_intent.content_context);
    case AYNI_INTENT_MEMORY_SAVE:
    case AYNI_INTENT_MEMORY_CONFIRM:
    case AYNI_INTENT_MEMORY_DELETE:
        return decode_memory_command(raw, out_intent.memory);
    case AYNI_INTENT_PAUSE_GROUP_SET:
        return read_pause_group_change(raw, out_intent.pause_group);
    case AYNI_INTENT_SCENE_SET_ENABLED:
        return read_scene_change(raw, out_intent.scene);
    case AYNI_INTENT_UNRESOLVED_OP_QUERY:
    case AYNI_INTENT_UNRESOLVED_OP_ACKNOWLEDGE:
        return read_target(raw.body, &ayni_unresolved_op_target_v1::logical_call_id,
                           out_intent.target_id);
    case AYNI_INTENT_CALL_QUERY_RESULT:
        return read_target(raw.body, &ayni_call_query_result_v1::logical_call_id,
                           out_intent.target_id);
    case AYNI_INTENT_TASK_RAISE_MODEL_LIMIT:
        return read_task_limit(raw, out_intent);
    case AYNI_INTENT_GOAL_CREATE:
        return read_goal_create(*static_cast<const ayni_goal_create_v1 *>(raw.body),
                                out_intent.create);
    case AYNI_INTENT_GOAL_REAUTHORIZE: {
        const ayni_goal_reauthorize_v1 *body = nullptr;
        const auto status = read_body(raw.body, body);
        if (status != AYNI_OK) {
            return status;
        }
        if (body->reserved != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        auto &request = out_intent.reauthorization;
        request.goal_id = body->goal_id;
        request.expected_authority_commit_seq = body->expected_authority_commit_seq;
        return read_goal_create(body->original, request.original);
    }
    case AYNI_INTENT_GOAL_APPROVE: {
        const ayni_goal_approve_v1 *body = nullptr;
        const auto status = read_body(raw.body, body);
        if (status != AYNI_OK) {
            return status;
        }
        if (body->reserved != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        auto &approval = out_intent.approval;
        approval.goal_id = body->goal_id;
        approval.missing_mask = body->missing_mask;
        approval.lifetime = body->lifetime;
        approval.duration_us = body->duration_us;
        return read_goal_create(body->original, approval.original);
    }
    case AYNI_INTENT_GRANT_CREATE: {
        const ayni_grant_create_v1 *body = nullptr;
        const ayni_status_t status = read_body(raw.body, body);
        if (status == AYNI_OK) {
            out_intent.grant_create = *body;
        }
        return status;
    }
    case AYNI_INTENT_GRANT_REVOKE:
        return read_target(raw.body, &ayni_grant_revoke_v1::grant_id, out_intent.target_id);
    case AYNI_INTENT_TASK_RESUME_WAIT:
    case AYNI_INTENT_TASK_CANCEL:
        return read_target(raw.body, &ayni_task_target_v1::task_id, out_intent.target_id);
    case AYNI_INTENT_INSTANCE_PAUSE:
    case AYNI_INTENT_INSTANCE_RESUME:
    case AYNI_INTENT_INSTANCE_STOP:
        return read_target(raw.body, &ayni_instance_target_v1::instance_id, out_intent.target_id);
    default: // GOAL_CANCEL, GOAL_PAUSE, GOAL_RESUME
        return read_target(raw.body, &ayni_goal_target_v1::goal_id, out_intent.target_id);
    }
}

} // namespace

ayni_status_t check_form(const Intent &intent) {
    // An exact command is recognised by its id, so it must have one.
    if (is_exact_command(intent.kind) && intent.command_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (is_context_command(intent.kind)) {
        uint32_t size = 0;
        return intent.content_context.kind != intent.kind ||
                       intent.content_context.expected_revision != intent.expected_revision
                   ? AYNI_INVALID_ARGUMENT
                   : measure_context_command(intent.content_context, size);
    }
    if (is_memory_command(intent.kind)) {
        uint32_t ignored = 0;
        return intent.command_id == 0 ? AYNI_INVALID_ARGUMENT
                                      : measure_memory_command(intent.memory, ignored);
    }
    if (intent.kind == AYNI_INTENT_PAUSE_GROUP_SET) {
        uint32_t size = 0;
        return intent.pause_group.expected_revision == intent.expected_revision
                   ? measure_pause_group_command(intent.pause_group, size)
                   : AYNI_INVALID_ARGUMENT;
    }
    if (intent.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
        return intent.scene.expected_revision != intent.expected_revision
                   ? AYNI_INVALID_ARGUMENT
                   : authority::validate_scene_change(intent.scene);
    }
    if (is_target_command(intent.kind)) {
        uint32_t size = 0;
        return measure_target_command({intent.kind, intent.expected_revision, intent.target_id},
                                      size);
    }
    if (intent.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT) {
        uint32_t size = 0;
        return measure_task_limit_command(
            {intent.expected_revision, intent.target_id, intent.model_call_limit}, size);
    }
    if (intent.kind == AYNI_INTENT_GOAL_REAUTHORIZE) {
        uint32_t size = 0;
        return measure_reauthorization_command({intent.expected_revision, intent.reauthorization},
                                               size);
    }
    if (intent.kind == AYNI_INTENT_GOAL_APPROVE) {
        uint32_t size = 0;
        return measure_approval_command({intent.expected_revision, intent.approval}, size);
    }
    if (intent.kind == AYNI_INTENT_GRANT_REVOKE) {
        return intent.expected_revision != 0U && intent.target_id != 0U ? AYNI_OK
                                                                        : AYNI_INVALID_ARGUMENT;
    }
    if (intent.kind == AYNI_INTENT_GRANT_CREATE) {
        uint32_t size = 0;
        return measure_authority_command({intent.expected_revision, &intent.grant_create}, size);
    }
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        return AYNI_OK;
    }
    return intent.expected_revision == 0U ? check_goal_form(intent.create) : AYNI_INVALID_ARGUMENT;
}

ayni_status_t decode_intent(const ayni_intent_v1 &raw, Intent &out_intent, IntentFailure &failure) {
    failure = IntentFailure{};
    ayni_status_t status = check_struct_size(raw.struct_size, sizeof(raw), sizeof(raw));
    if (status != AYNI_OK) {
        return status;
    }
    if (raw.kind == 0U || raw.reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!implements_intent(raw.kind)) {
        failure.reason = AYNI_REASON_INTENT_UNSUPPORTED;
        return AYNI_UNSUPPORTED;
    }
    if (raw.body == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    out_intent = Intent{};
    out_intent.kind = raw.kind;
    out_intent.command_id = raw.command_id;
    out_intent.session_id = raw.session_id;
    out_intent.expected_revision = raw.expected_revision;
    status = decode_body(raw, out_intent);
    if (status == AYNI_OK) {
        status = check_form(out_intent);
    }
    return status;
}

uint64_t ordinary_command_digest(const Intent &intent) {
    foundation::Digest digest;
    digest.add_u32(intent.kind);
    digest.add_u32(intent.expected_revision);
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        digest.add_u64(intent.target_id);
        return digest.value();
    }
    const GoalCreate &create = intent.create;
    digest.add_bytes(create.goal_type_id.data, create.goal_type_id.size);
    digest.add_u32(create.goal_type_version);
    digest.add_u64(create.owner_instance_id);
    digest.add_u64(create.parent_goal_id);
    digest.add_bytes(create.parameters.data, create.parameters.size);
    digest.add_u32(create.trigger.kind);
    digest.add_u64(create.trigger.delay_us);
    digest.add_u64(create.trigger.period_us);
    digest.add_u64(create.expiry_us);
    digest.add_bytes(create.trigger_source.data, create.trigger_source.size);
    if (create.calendar.kind != 0) {
        digest.add_u32(create.calendar.kind);
        digest.add_u32(create.calendar.version);
        digest.add_u64(static_cast<uint64_t>(create.calendar.first_local_s));
        digest.add_bytes(create.calendar.timezone, create.calendar.timezone_size);
    }
    // Preserve every old ungrouped command digest across this development upgrade.
    if (create.pause_group != 0) {
        digest.add_u32(create.pause_group);
    }
    return digest.value();
}

} // namespace ayni::kernel
