#include "motor.h"
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/scenes.h>
#include <cstdio>
#include <cstring>
namespace wifichat {
namespace {
constexpr uint32_t version = motor_version, request_timeout_ms = 5000;
enum Tool : size_t { status_tool, run_tool, stop_tool, stream_tool, steer_tool, execute_tool };
constexpr const char *identities[]{"motor.status", "motor.run",   "motor.stop",
                                   "motor.stream", "motor.steer", stream_execute};
constexpr const char *summaries[]{
    "Read local motor readiness and the independently authorized active session identity.",
    "Run one bounded multi-segment PWM trajectory and await its actual output-stop result.",
    "Recoverable soft-stop of motor output; confirms GPIO disable and keeps local arming.",
    "Open an independently authorized streaming session, surviving the originating chat turn.",
    "Submit a new target inside an active motor.stream envelope; receipt is not application.",
    "Execute an independently authorized local streaming session (runtime-owned route)."};
constexpr const char *parameters[]{
    "UNARY invoke, empty object. Wiring/power and wheel state are not measured.",
    "ACTION invoke. segments: 1..8 ordered {motor1_percent,motor2_percent,duration_ms}; "
    "percent -90..90, ms 50..60000 multiple of 5, cumulative <=60000. Zero segments coast. "
    "No motor.stop between ordinary segments. This is PWM, not RPM or distance.",
    "UNARY invoke, empty object. Soft-stop, no hardware power-off or wheel standstill claim.",
    "UNARY invoke. Required integer motor1_min,motor1_max,motor2_min,motor2_max (-90..90); "
    "each range must include zero. motor1_percent,motor2_percent initially inside ranges; "
    "duration_ms 50..60000 multiple of 5 bounds the entire independent session. Returns "
    "session_id, boot, generation; independent control authorization is required. Natural "
    "new instructions default to hold; explicit cancel/stop terminates the session.",
    "UNARY invoke. Required session_id (from motor.stream), motor1_percent,motor2_percent "
    "inside its envelope, duration_ms 50..60000 multiple of 5. May shorten the remaining "
    "session deadline; never extends its original lifetime. Accepted means queued; the "
    "independent control feedback reports actual application/rejection. No re-handshake.",
    ("Internal ACTION route. Same canonical envelope input as motor.stream; only its "
     "independent Goal/Task may dispatch this registered Provider route.")};
constexpr const char *examples[]{
    "{}",
    "{\"segments\":[{\"motor1_percent\":40,\"motor2_percent\":40,\"duration_ms\":1000}]}",
    "{}",
    ("{\"motor1_min\":-60,\"motor1_max\":60,\"motor2_min\":-60,\"motor2_max\":60,\"motor1_"
     "percent\":"
     "40,\"motor2_percent\":40,\"duration_ms\":5000}"),
    "{\"session_id\":1,\"motor1_percent\":20,\"motor2_percent\":30,\"duration_ms\":1000}",
    "{}"};
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && a.data && !std::memcmp(a.data, b, a.size);
}
bool same_attempt(ayni_attempt_ref_v1 a, ayni_attempt_ref_v1 b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}
ayni_status_t reply(ayni_provider_completion_t *completion, ayni_attempt_ref_v1 attempt,
                    ayni_status_t code, const char *body) {
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = attempt;
    result.outcome = code == AYNI_OK ? AYNI_CALL_COMPLETED : AYNI_CALL_FAILED;
    result.status = code;
    result.effect = code == AYNI_OK ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_NONE;
    if (body) {
        result.result = text(body);
    }
    return ayni_provider_post(completion, &result);
}
} // namespace

ayni_status_t Motor::configure(ayni_kernel_t *kernel) {
    status_input_.kind = AYNI_VALUE_OBJECT;
    status_input_.subtree_end = 1;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = motor_reply_bytes_max;
    business_[status_tool] = {{&status_input_, 1}, {&result_, 1}, 0, 0, nullptr};
    business_[stop_tool] = business_[status_tool];
    run_input_ = motor_run_schema();
    stream_input_ = motor_stream_schema();
    steer_input_[0].kind = AYNI_VALUE_OBJECT;
    steer_input_[0].subtree_end = uint32_t(steer_input_.size());
    for (uint32_t at = 0; at < projections_.size(); ++at) {
        projections_[at].field_id = at + 1;
        projections_[at].path_count = 3;
        projections_[at].path[0] = 1;
        projections_[at].path[1] = AYNI_PROJECTION_LIST_ITEM;
        projections_[at].path[2] = at + 1;
        projections_[at].unit = run_input_[at + 3].unit;
        steer_input_[at + 1] = run_input_[at + 3];
        steer_input_[at + 1].subtree_end = at + 2;
        steer_projections_[at].field_id = at + 1;
        steer_projections_[at].path_count = 1;
        steer_projections_[at].path[0] = at + 1;
        steer_projections_[at].unit = run_input_[at + 3].unit;
    }
    auto &session_id = steer_input_.back();
    session_id.kind = AYNI_VALUE_U64;
    session_id.field_id = 4;
    session_id.subtree_end = uint32_t(steer_input_.size());
    session_id.flags = AYNI_SCHEMA_REQUIRED;
    session_id.name = text("session_id");
    session_id.min_u64 = 1;
    session_id.max_u64 = UINT64_MAX;
    for (uint32_t at = 0; at < stream_projections_.size(); ++at) {
        auto &p = stream_projections_[at];
        p.field_id = at + 1;
        p.path_count = 1;
        p.path[0] = at + 1;
        p.unit = stream_input_[at + 1].unit;
    }
    business_[run_tool] = {{run_input_.data(), uint32_t(run_input_.size())},
                           {&result_, 1},
                           uint32_t(projections_.size()),
                           0,
                           projections_.data()};
    business_[stream_tool] = {{stream_input_.data(), uint32_t(stream_input_.size())},
                              {&result_, 1},
                              uint32_t(stream_projections_.size()),
                              0,
                              stream_projections_.data()};
    business_[execute_tool] = business_[stream_tool];
    business_[steer_tool] = {{steer_input_.data(), uint32_t(steer_input_.size())},
                             {&result_, 1},
                             uint32_t(steer_projections_.size()),
                             0,
                             steer_projections_.data()};
    resource_ = {text("motor.actuator"), 1, 0};
    for (size_t at = 0; at < offers_.size(); ++at) {
        const bool stream = at == stream_tool || at == execute_tool;
        const uint32_t fields_count = stream                               ? stream_field_count
                                      : at == run_tool || at == steer_tool ? 3U
                                                                           : 0U;
        std::array<ayni_authority_field_desc_v1, stream_field_count> fields{};
        for (uint32_t field = 0; field < fields_count; ++field) {
            const auto &node = stream ? stream_input_[field + 1] : run_input_[field + 3];
            auto &f = fields[field];
            f.struct_size = sizeof(f);
            f.field_id = field + 1;
            f.usage = AYNI_AUTHORITY_PARAMETER;
            f.domain.struct_size = sizeof(f.domain);
            f.domain.kind = AYNI_AUTHORITY_I64;
            f.domain.unit = node.unit;
            f.domain.values.integer = {node.min_i64, node.max_i64};
        }
        const ayni_capability_desc_v1 authority{
            sizeof(authority), version, text(identities[at]),
            fields_count,      0,       fields_count ? fields.data() : nullptr};
        auto rc = ayni_register_capability(kernel, &authority);
        if (rc != AYNI_OK) {
            return rc;
        }
        auto &offer = offers_[at];
        offer = {sizeof(offer),
                 version,
                 text(identities[at]),
                 {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                  AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_NONE, 0,
                  request_timeout_ms, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT,
                  motor_input_bytes_max, motor_reply_bytes_max, 0, nullptr, nullptr,
                  AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                 &business_[at]};
        if (at == run_tool || at == execute_tool) {
            offer.contract.form = AYNI_CALL_ACTION;
            offer.contract.effects = AYNI_CALL_EFFECT_PHYSICAL;
            offer.contract.idempotency = AYNI_CALL_NOT_IDEMPOTENT;
            offer.contract.cancel = AYNI_CALL_CANCEL_REQUEST;
            offer.contract.timeout_ms = 65000;
            offer.contract.resource_count = 1;
            offer.contract.resources = &resource_;
        } else if (at == stream_tool) {
            offer.contract.idempotency = AYNI_CALL_NOT_IDEMPOTENT;
            offer.contract.cancel = AYNI_CALL_CANCEL_REQUEST;
        }
        rc = ayni_register_call_capability(kernel, &offer);
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 text("motor.local"),
                 text("motor.ta6586"),
                 {},
                 this,
                 uint32_t(offers_.size()),
                 0,
                 offers_.data(),
                 submit,
                 cancel,
                 nullptr,
                 nullptr};
    auto rc = ayni_register_provider(kernel, &provider_);
    for (size_t at = 0; rc == AYNI_OK && at < offers_.size(); ++at) {
        const ayni_binding_v1 binding{sizeof(binding),
                                      version,
                                      text(identities[at]),
                                      text(identities[at]),
                                      version,
                                      version,
                                      provider_.provider_id,
                                      provider_.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        rc = ayni_register_binding(kernel, &binding);
        if (rc != AYNI_OK) {
            return rc;
        }
        const bool stream = at == stream_tool || at == execute_tool;
        const uint32_t count = stream                               ? stream_field_count
                               : at == run_tool || at == steer_tool ? 3U
                                                                    : 0U;
        std::array<ayni_tool_field_usage_v1, stream_field_count> descriptions{};
        for (uint32_t field = 0; field < count; ++field) {
            descriptions[field] = {sizeof(ayni_tool_field_usage_v1), field + 1,
                                   stream ? stream_input_[field + 1].name
                                          : run_input_[field + 3].name};
        }
        const ayni_tool_usage_v1 usage{
            sizeof(usage),
            version,
            binding.binding_id,
            version,
            binding.capability_id,
            text(summaries[at]),
            text(parameters[at]),
            text("Controller PWM only; wheel direction/RPM/distance remain unverified. Local "
                 "manual bench; independent hardware watchdog/reset-disable remain unverified."),
            text(examples[at]),
            count,
            0,
            count ? descriptions.data() : nullptr};
        rc = ayni_register_capability_usage(kernel, text(summaries[at]), &usage);
    }
    const ayni_resource_desc_v1 resource{sizeof(resource),
                                         AYNI_RESOURCE_EXCLUSIVE,
                                         resource_.resource_id,
                                         1,
                                         AYNI_RESOURCE_ACTUATOR,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
    if (rc == AYNI_OK) {
        rc = ayni_register_resource(kernel, &resource);
    }
    if (rc == AYNI_OK) {
        rc = ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE);
    }
    const auto ports = execution_.ports();
    if (rc == AYNI_OK) {
        rc = ayni_register_executor(kernel, &ports);
    }
    const std::array<ayni_scene_rule_v1, 2> rules{
        {{text("motor.run"), version, 0, {sizeof(ayni_scope_v1), 0, nullptr}},
         {text(stream_execute), version, 0, {sizeof(ayni_scope_v1), 0, nullptr}}}};
    const ayni_scene_desc_v1 scene{sizeof(scene),
                                   motor_scene_version,
                                   text(motor_scene),
                                   uint32_t(rules.size()),
                                   0,
                                   rules.data(),
                                   nullptr};
    if (rc == AYNI_OK) {
        rc = ayni_register_scene(kernel, &scene);
    }
    for (const bool streaming : {false, true}) {
        std::array<ayni_execution_field_v1, 3> mappings{};
        constexpr uint32_t channel_order[]{2, 0, 1};
        for (uint32_t at = 0; at < mappings.size(); ++at) {
            const auto channel = channel_order[at];
            const auto minimum = !streaming     ? channel + 1
                                 : channel == 2 ? stream_duration
                                 : channel == 0 ? stream_m1_min
                                                : stream_m2_min;
            const auto maximum = !streaming     ? channel + 1
                                 : channel == 2 ? stream_duration
                                 : channel == 0 ? stream_m1_max
                                                : stream_m2_max;
            mappings[at] = {text(motor_fields[channel]), text(channel == 2 ? "ms" : "percent"),
                            minimum, maximum};
        }
        const ayni_execution_binding_v1 execution{sizeof(execution),
                                                  version,
                                                  text(streaming ? stream_execute : "motor.run"),
                                                  ports.executor_id,
                                                  version,
                                                  motor_scene_version,
                                                  text(motor_scene),
                                                  AYNI_FORM_SESSION,
                                                  motor_session_lease_us,
                                                  motor_sample_age_us,
                                                  uint32_t(mappings.size()),
                                                  mappings.data()};
        if (rc == AYNI_OK) {
            rc = ayni_register_execution_binding(kernel, &execution);
        }
    }
    return rc;
}

ayni_submit_result_t Motor::submit(void *context, const ayni_provider_request_v1 *request,
                                   ayni_provider_completion_t *completion) {
    if (!request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    auto &self = *static_cast<Motor *>(context);
    if (same(request->capability_id, identities[run_tool])) {
        return self.execution_.submit(request, completion);
    }
    if (same(request->capability_id, identities[stop_tool])) {
        return self.execution_.submit_soft_stop(request, completion);
    }
    if (same(request->capability_id, identities[stream_tool])) {
        return self.open_session(*request, completion);
    }
    if (same(request->capability_id, identities[steer_tool])) {
        return self.steer(*request, completion);
    }
    if (same(request->capability_id, stream_execute)) {
        MotorStreamInput input;
        if (self.execute_ || !request->execution_permit ||
            !read_motor_stream(request->value, input)) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.execute_ = Execute{request->attempt, completion, request->logical_call_id,
                                *request->execution_permit, input.initial};
        return AYNI_SUBMIT_ACCEPTED; // Owner validates the exact independent Task before hardware
                                     // admission.
    }
    if (!same(request->capability_id, identities[status_tool])) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    char state[motor_reply_bytes_max], hardware[768];
    self.execution_.describe(hardware, sizeof(hardware));
    const auto session = self.activity_.snapshot();
    std::snprintf(state, sizeof(state),
                  "{\"controller\":%s,\"session_id\":%llu,\"session_phase\":%u}", hardware,
                  (unsigned long long)session.handle.goal, unsigned(session.phase));
    return reply(completion, request->attempt, AYNI_OK, state) == AYNI_OK
               ? AYNI_SUBMIT_ACCEPTED
               : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t Motor::cancel(void *context, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Motor *>(context);
    if (self.opening_ && same_attempt(self.opening_->attempt, attempt)) {
        self.opening_->phase = Opening::Phase::rollback;
        self.activity_.close_all();
        return AYNI_SUBMIT_ACCEPTED;
    }
    if (self.steering_ && same_attempt(self.steering_->attempt, attempt)) {
        if (!self.steering_->seq) {
            self.steering_->status = AYNI_DENIED;
        }
        return AYNI_SUBMIT_ACCEPTED; // Already queued commands are not replayed or falsely undone.
    }
    if (self.execute_ && same_attempt(self.execute_->attempt, attempt)) {
        self.execute_->state = Execute::State::cancelled;
        return AYNI_SUBMIT_ACCEPTED;
    }
    return self.execution_.cancel(attempt);
}
} // namespace wifichat
