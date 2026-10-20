#include "kernel/intent/intent.h"

#include "catalog/catalog_id.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "kernel/abi_checks.h"

namespace ayni::kernel {
namespace {

// Each body struct is new in 0.5 or later or changed since, so each has one
// layout.
template <typename Body> ayni_status_t read_body(const void *body, const Body *&out_body) {
    out_body = static_cast<const Body *>(body);
    return check_struct_size(out_body->struct_size, sizeof(Body), sizeof(Body));
}

// The body of every kind but GOAL_CREATE names one object by id.
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
    return kind >= AYNI_INTENT_GOAL_CREATE && kind <= AYNI_INTENT_INSTANCE_STOP;
}

ayni_status_t check_form(const Intent &intent) {
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
    out_intent.session_id = raw.session_id;
    out_intent.expected_revision = raw.expected_revision;
    status = decode_body(raw, out_intent);
    if (status == AYNI_OK) {
        status = check_form(out_intent);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (raw.command_id != 0U) {
        failure.reason = AYNI_REASON_COMMAND_ID_UNSUPPORTED;
        return AYNI_UNSUPPORTED;
    }
    return AYNI_OK;
}

} // namespace ayni::kernel
