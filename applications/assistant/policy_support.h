#ifndef AYNI_ASSISTANT_POLICY_SUPPORT_H
#define AYNI_ASSISTANT_POLICY_SUPPORT_H
#include <ayni/applications/assistant.h>
#include <cstring>
namespace ayni::applications::assistant::detail {
inline ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
inline ayni_string_v1 text(const std::string &s) {
    return {s.data(), static_cast<uint32_t>(s.size())};
}
inline bool taken(const ayni_task_snapshot_v1 &s, ayni_string_v1 key) {
    for (uint32_t i = 0; i < s.step_count; ++i) {
        if (s.steps[i].step_key.size == key.size &&
            std::memcmp(s.steps[i].step_key.data, key.data, key.size) == 0) {
            return true;
        }
    }
    return false;
}
inline ayni_status_t report(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &o,
                            ayni_policy_intent_kind_t kind, ayni_string_v1 key = {}) {
    if (!o.intents || !o.intent_capacity || key.size > AYNI_CATALOG_ID_MAX_BYTES) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    o.intents[0] = {};
    auto &i = o.intents[0];
    i.struct_size = sizeof(i);
    i.kind = kind;
    i.expected_revision = s.revision;
    i.step_key_size = key.size;
    if (key.size) {
        std::memcpy(i.step_key, key.data, key.size);
    }
    o.intent_count = 1;
    return AYNI_OK;
}
inline ayni_status_t request(ayni_policy_output_v1 &o, const char *binding, ayni_string_v1 key,
                             ayni_string_v1 body, ayni_scope_v1 scope, bool model = false) {
    if (!o.request_call) {
        return AYNI_UNSUPPORTED;
    }
    const ayni_call_request_v1 r{
        sizeof(r), model ? AYNI_POLICY_REQUEST_MODEL : AYNI_POLICY_REQUEST_CALL,
        1,         0,
        key,       text(binding),
        scope,     body};
    return o.request_call(o.call_context, &r);
}
// The whole POSIX TZ rule must parse. Converting one day after the epoch validates it without
// rejecting a legitimate west offset, whose local time at epoch 0 would be negative.
inline ayni_status_t check_timezone(ayni_string_v1 timezone) {
    constexpr int64_t validation_utc_s = 86400;
    int64_t local = 0;
    return ayni_calendar_local_time(timezone, validation_utc_s, &local);
}
inline ayni_scope_v1 empty_scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
// Failed/unknown calls hold the original Task; no retry is inferred from a policy decision.
inline ayni_status_t result(const ayni_task_snapshot_v1 &s, ayni_string_v1 key,
                            ayni_task_call_result_v1 &r) {
    if (!s.call_result) {
        return AYNI_UNSUPPORTED;
    }
    r = {};
    r.struct_size = sizeof(r);
    const auto status = s.call_result(s.call_context, key, &r);
    if (status != AYNI_OK) {
        return status;
    }
    if (r.outcome == AYNI_CALL_ABANDONED || r.status == AYNI_RESULT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (!r.outcome) {
        return AYNI_BUSY;
    }
    if (r.effect == AYNI_CALL_EFFECT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (r.outcome != AYNI_CALL_COMPLETED || r.status != AYNI_OK) {
        return r.status == AYNI_OK ? AYNI_INVALID_STATE : r.status;
    }
    return AYNI_OK;
}
inline bool confirmed_cancel(const ayni_task_call_result_v1 &call) {
    return call.outcome == AYNI_CALL_CANCELLED && call.status == AYNI_OK &&
           (call.effect == AYNI_CALL_EFFECT_NONE || call.effect == AYNI_CALL_EFFECT_PARTIAL ||
            call.effect == AYNI_CALL_EFFECT_COMPLETE);
}
// Every retained ID of one kind, never a partial list (C-LIFE-030). Owner thread.
ayni_status_t objects(const ayni_kernel_t *, ayni_object_kind_t, std::vector<uint64_t> &);
ayni_status_t organize(const Parameters &, const PolicyConfig &, const ayni_task_snapshot_v1 &,
                       ayni_policy_output_v1 &);
ayni_status_t observe(const PolicyConfig &, const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &);
} // namespace ayni::applications::assistant::detail
#endif
