#include "kernel/intent/intent.h"

#include "catalog/catalog_id.h"
#include "kernel/abi_checks.h"

namespace ayni::kernel {
namespace {

// The body structs are new in 0.5, so each has one layout.
template <typename Body> ayni_status_t read_body(const void *body, const Body *&out_body) {
    out_body = static_cast<const Body *>(body);
    return check_struct_size(out_body->struct_size, sizeof(Body), sizeof(Body));
}

ayni_status_t decode_body(const ayni_intent_v1 &raw, Intent &out_intent) {
    if (raw.kind == AYNI_INTENT_GOAL_CREATE) {
        const ayni_goal_create_v1 *body = nullptr;
        const ayni_status_t status = read_body(raw.body, body);
        if (status != AYNI_OK) {
            return status;
        }
        out_intent.create = {body->goal_type_id, body->goal_type_version, body->owner_instance_id,
                             body->parent_goal_id, body->parameters};
        return AYNI_OK;
    }
    const ayni_goal_target_v1 *body = nullptr;
    const ayni_status_t status = read_body(raw.body, body);
    if (status != AYNI_OK) {
        return status;
    }
    if (body->reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    out_intent.goal_id = body->goal_id;
    return AYNI_OK;
}

} // namespace

bool is_implemented(ayni_intent_kind_t kind) {
    return kind == AYNI_INTENT_GOAL_CREATE || kind == AYNI_INTENT_GOAL_CANCEL ||
           kind == AYNI_INTENT_GOAL_PAUSE || kind == AYNI_INTENT_GOAL_RESUME;
}

ayni_status_t check_form(const Intent &intent) {
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        return AYNI_OK;
    }
    const GoalCreate &create = intent.create;
    if (intent.expected_revision != 0U || !catalog::CatalogId::is_valid(create.goal_type_id) ||
        (create.parameters.data == nullptr && create.parameters.size != 0U)) {
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
