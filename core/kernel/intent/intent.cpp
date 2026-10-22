#include "kernel/intent/intent.h"

#include "catalog/catalog_id.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "foundation/digest.h"
#include "kernel/abi_checks.h"
#include "kernel/command/command_canonical.h"

namespace ayni::kernel {
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

ayni_status_t decode_body(const ayni_intent_v1 &raw, Intent &out_intent) {
    switch (raw.kind) {
    case AYNI_INTENT_GOAL_CREATE: {
        const ayni_goal_create_v1 *body = nullptr;
        const ayni_status_t status = read_body(raw.body, body);
        if (status != AYNI_OK) {
            return status;
        }
        if (body->reserved != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        out_intent.create = {body->goal_type_id,
                             body->goal_type_version,
                             body->owner_instance_id,
                             body->parent_goal_id,
                             body->parameters,
                             {body->trigger, body->trigger_delay_us, body->trigger_period_us},
                             body->expiry_us,
                             body->trigger_source};
        return AYNI_OK;
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

// A local input trigger names its input by a catalog id; other kinds take
// none.
bool trigger_source_fits(const GoalCreate &create) {
    return create.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT
               ? catalog::CatalogId::is_valid(create.trigger_source)
               : create.trigger_source.size == 0U;
}

} // namespace

bool is_implemented(ayni_intent_kind_t kind) {
    return kind >= AYNI_INTENT_GOAL_CREATE && kind <= AYNI_INTENT_GRANT_REVOKE;
}

ayni_status_t check_form(const Intent &intent) {
    if (intent.kind == AYNI_INTENT_GRANT_REVOKE) {
        return intent.command_id != 0U && intent.expected_revision != 0U && intent.target_id != 0U
                   ? AYNI_OK
                   : AYNI_INVALID_ARGUMENT;
    }
    if (intent.kind == AYNI_INTENT_GRANT_CREATE) {
        if (intent.command_id == 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        uint32_t size = 0;
        return measure_authority_command({intent.expected_revision, &intent.grant_create}, size);
    }
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        return AYNI_OK;
    }
    const GoalCreate &create = intent.create;
    if (intent.expected_revision != 0U || !catalog::CatalogId::is_valid(create.goal_type_id) ||
        (create.parameters.data == nullptr && create.parameters.size != 0U) ||
        !catalog::GoalTypeRegistry::defines_trigger(create.trigger.kind) ||
        !create.trigger.fits() || !trigger_source_fits(create)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
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
    if (!is_implemented(raw.kind)) {
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
    return digest.value();
}

} // namespace ayni::kernel
