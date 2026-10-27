#include <array>
#include <ayni/providers/mock_execution.h>
#include <cstring>
#include <new>
#include <optional>
#include <utility>
namespace ayni::providers::mock_execution {
namespace {
constexpr uint32_t version = 1;
constexpr uint64_t domain = 1;
constexpr uint32_t duration_max_us = 1000000;
constexpr uint32_t sample_age_max_us = 100000;
constexpr uint32_t poll_period_us = 20000;
constexpr uint32_t text_bytes = 64;
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.data != nullptr && a.size == b.size && std::memcmp(a.data, b.data, a.size) == 0;
}
bool same(const ayni_permit_epochs_v1 &a, const ayni_permit_epochs_v1 &b) {
    return a.kernel_boot_epoch == b.kernel_boot_epoch && a.authority_epoch == b.authority_epoch &&
           a.executor_boot_epoch == b.executor_boot_epoch && a.safety_epoch == b.safety_epoch &&
           a.calibration_version == b.calibration_version && a.reserved == b.reserved;
}
} // namespace
struct Executor::Impl {
    // One executor request kept while answers are held; inputs are copies.
    struct Request {
        ayni_executor_request_t kind;
        ayni_attempt_ref_v1 identity;
        ayni_executor_completion_t *completion;
        ayni_permit_epochs_v1 epochs;   // CHALLENGE.
        ayni_permit_renewal_v1 renewal; // RENEW.
        uint64_t permit_id;             // STOP and STATUS.
    };
    static constexpr uint32_t held_max = 8; // A full link refuses with BUSY.
    struct Pending {
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        uint32_t remaining;
        enum class Outcome { running, success, cancelled } outcome = Outcome::running;
        bool produced = false;             // Whether this operation emitted a permitted sample.
        ayni_execution_permit_v1 permit{}; // Copied at submit.
        bool started = false;              // Whether the gate accepted the permit.
    };
    Config config;
    std::unique_ptr<uint64_t[]> memory;
    size_t memory_bytes = 0;
    ayni_control_gate_t *gate = nullptr;
    ayni_control_observation_v1 live{};
    ayni_execution_permit_v1 permit{};
    ayni_call_resource_v1 resource{text("mock.actuator"), 1, 0};
    ayni_call_capability_v1 capability{};
    ayni_provider_v1 provider{};
    ayni_executor_v1 executor{};
    std::optional<Pending> pending;
    std::array<Request, held_max> held{};
    uint32_t held_count = 0;
    uint64_t refused_through = 0; // Highest permit a status read answered NOT_FOUND.
    uint64_t sample_count = 0;
    uint64_t renewal_count = 0;
    uint64_t submit_count = 0;
    uint64_t last_call = 0;
    uint64_t sequence = 0;
    ayni_execution_target_t target = AYNI_TARGET_UNVERIFIED;
    uint64_t now() const { return config.now_us(config.clock_context); }
    void describe() {
        capability = {sizeof(capability),
                      version,
                      text("mock.turn"),
                      {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL,
                       AYNI_CALL_EFFECT_PHYSICAL, AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE,
                       AYNI_CALL_CANCEL_REQUEST, 0, config.call_timeout_ms,
                       AYNI_CALL_INPUT_FIELDS_TEXT, AYNI_CALL_RESULT_TEXT, text_bytes, text_bytes,
                       1, &resource, nullptr}};
        provider = {sizeof(provider),
                    version,
                    text("mock.execution"),
                    text("mock.control"),
                    {},
                    this,
                    1,
                    0,
                    &capability,
                    submit,
                    cancel,
                    nullptr,
                    nullptr};
        executor = {sizeof(executor),
                    version,
                    text("mock.executor"),
                    domain,
                    config.executor_boot_epoch,
                    1,
                    1,
                    1,
                    duration_max_us,
                    duration_max_us,
                    sample_age_max_us,
                    poll_period_us,
                    this,
                    request_challenge,
                    request_renew,
                    request_stop,
                    request_status};
    }
    // The trusted Core supplies its new boot at the first challenge. This gate is
    // never recreated or rebound after it has been configured for that generation.
    ayni_status_t challenge(const ayni_permit_epochs_v1 &expected, ayni_executor_nonce_v1 &out) {
        if (expected.executor_boot_epoch != config.executor_boot_epoch ||
            expected.safety_epoch != 1 || expected.calibration_version != 1) {
            return AYNI_DENIED;
        }
        if (gate == nullptr) {
            const ayni_control_config_v1 settings{
                sizeof(settings), AYNI_ABI_MAJOR,  AYNI_ABI_MINOR,    1, domain,       expected,
                duration_max_us,  duration_max_us, sample_age_max_us, 0, config.limits};
            const auto created =
                ayni_control_create(&settings, now(), memory.get(), memory_bytes, &gate);
            if (created != AYNI_OK) {
                return created;
            }
            live = {expected, 1, 0};
        } else if (!same(expected, live.epochs)) {
            return AYNI_STALE;
        }
        return ayni_control_nonce(gate, now(), &out);
    }
    ayni_status_t renew(const ayni_permit_renewal_v1 &renewal) {
        if (gate == nullptr) {
            return AYNI_NOT_READY;
        }
        const auto status = ayni_control_renew(gate, &renewal, now(), &live);
        if (status == AYNI_OK) {
            ++renewal_count;
        }
        return status;
    }
    ayni_status_t stop(uint64_t permit_id) {
        return gate == nullptr ? AYNI_NOT_FOUND : ayni_control_stop(gate, permit_id, now());
    }
    // NOT_FOUND fences the permit: submit refuses it from then on.
    ayni_status_t status(uint64_t permit_id, ayni_executor_status_v1 &out) {
        if (gate == nullptr || permit_id > permit.permit_id) {
            refused_through = permit_id > refused_through ? permit_id : refused_through;
            return AYNI_NOT_FOUND;
        }
        if (permit_id != permit.permit_id) {
            return AYNI_STALE;
        }
        // check updates expiry/interlocks; its denial is represented by the exact copied state.
        const auto evaluated = ayni_control_check(gate, now(), &live);
        if (evaluated == AYNI_INVALID_ARGUMENT) {
            return evaluated;
        }
        out.struct_size = sizeof(out);
        out.target = target;
        out.control.struct_size = sizeof(out.control);
        return ayni_control_get_info(gate, &out.control);
    }
    // Serves one request against the gate as it is now and posts the answer.
    ayni_status_t serve(const Request &request) {
        ayni_executor_response_v1 response{};
        response.struct_size = sizeof(response);
        response.kind = request.kind;
        response.request = request.identity;
        switch (request.kind) {
        case AYNI_EXECUTOR_CHALLENGE:
            response.status = challenge(request.epochs, response.output.nonce);
            break;
        case AYNI_EXECUTOR_RENEW:
            response.status = renew(request.renewal);
            break;
        case AYNI_EXECUTOR_STOP:
            response.status = stop(request.permit_id);
            break;
        default:
            response.status = status(request.permit_id, response.output.status);
            break;
        }
        return ayni_executor_post(request.completion, &response);
    }
    // Immediate answers are posted inside the port; held ones wait for answer().
    static ayni_status_t take(void *context, const Request &request) {
        auto &self = *static_cast<Impl *>(context);
        if (request.completion == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (self.config.answers == Answers::immediate) {
            return self.serve(request) == AYNI_OK ? AYNI_OK : AYNI_INTERNAL;
        }
        if (self.held_count == held_max) {
            return AYNI_BUSY;
        }
        self.held[self.held_count++] = request;
        return AYNI_OK;
    }
    static ayni_status_t request_challenge(void *context, ayni_attempt_ref_v1 identity,
                                           const ayni_permit_epochs_v1 *expected,
                                           ayni_executor_completion_t *completion) {
        if (expected == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        return take(context, {AYNI_EXECUTOR_CHALLENGE, identity, completion, *expected, {}, 0});
    }
    static ayni_status_t request_renew(void *context, ayni_attempt_ref_v1 identity,
                                       const ayni_permit_renewal_v1 *renewal,
                                       ayni_executor_completion_t *completion) {
        if (renewal == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        return take(context, {AYNI_EXECUTOR_RENEW, identity, completion, {}, *renewal, 0});
    }
    static ayni_status_t request_stop(void *context, ayni_attempt_ref_v1 identity,
                                      uint64_t permit_id, ayni_executor_completion_t *completion) {
        return take(context, {AYNI_EXECUTOR_STOP, identity, completion, {}, {}, permit_id});
    }
    static ayni_status_t request_status(void *context, ayni_attempt_ref_v1 identity,
                                        uint64_t permit_id,
                                        ayni_executor_completion_t *completion) {
        return take(context, {AYNI_EXECUTOR_STATUS, identity, completion, {}, {}, permit_id});
    }
    ayni_status_t answer_held() {
        ayni_status_t first = AYNI_OK;
        for (uint32_t at = 0; at < held_count; ++at) {
            const auto posted = serve(held[at]);
            if (first == AYNI_OK) {
                first = posted;
            }
        }
        held_count = 0;
        return first;
    }
    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (request == nullptr || completion == nullptr ||
            request->struct_size != sizeof(*request) || request->capability_version != version ||
            !same(request->capability_id, self.capability.capability_id) ||
            !same(request->endpoint_id, self.provider.endpoint_id) ||
            request->execution_permit == nullptr || request->attempt.boot_epoch == 0 ||
            request->attempt.request_id == 0 || request->logical_call_id == 0 ||
            request->execution_permit->operation_id != request->operation_id ||
            request->execution_permit->permit_id <= self.refused_through || self.gate == nullptr ||
            self.pending) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        const bool now_at_gate = self.config.permit_start == PermitStart::at_submit;
        if (now_at_gate && ayni_control_start(self.gate, request->execution_permit, self.now(),
                                              &self.live) != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.pending = Pending{request->attempt,
                               completion,
                               self.config.ticks_to_complete,
                               Pending::Outcome::running,
                               false,
                               *request->execution_permit,
                               false};
        if (now_at_gate) {
            self.accept_permit();
        }
        self.last_call = request->logical_call_id;
        ++self.submit_count;
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        if (!self.pending || self.pending->attempt.boot_epoch != attempt.boot_epoch ||
            self.pending->attempt.request_id != attempt.request_id) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        if (self.pending->outcome == Pending::Outcome::running) {
            self.pending->outcome = Pending::Outcome::cancelled;
        }
        if (!self.pending->started) {
            return AYNI_SUBMIT_ACCEPTED; // The gate never had it.
        }
        return self.stop(self.permit.permit_id) == AYNI_OK ? AYNI_SUBMIT_ACCEPTED
                                                           : AYNI_SUBMIT_ACCEPT_UNKNOWN;
    }
    void accept_permit() {
        permit = pending->permit;
        pending->started = true;
        sequence = 0;
        target = AYNI_TARGET_UNVERIFIED;
    }
    ayni_status_t measure_stop() {
        if (gate == nullptr) {
            return AYNI_NOT_READY;
        }
        return ayni_control_confirm_stopped(gate, permit.permit_id, now());
    }
    ayni_status_t sample() {
        ayni_control_sample_v1 value{};
        value.struct_size = sizeof(value);
        value.count = permit.envelope.count;
        value.permit_id = permit.permit_id;
        value.sequence = ++sequence;
        value.sampled_at_us = now();
        for (uint32_t at = 0; at < value.count; ++at) {
            const auto &range = permit.envelope.fields[at];
            value.fields[at] = {range.field_id, range.unit, range.minimum};
        }
        const auto status = ayni_control_sample(gate, &value, now(), &live);
        if (status == AYNI_OK) {
            ++sample_count;
            pending->produced = true;
        }
        return status;
    }
    // Independent stop measurement may precede or follow the terminal. A
    // successful local output never implies stopped or target reached by itself.
    ayni_status_t advance() {
        if (gate == nullptr) {
            return AYNI_OK;
        }
        // A forwarded permit reaches the gate now, unless a NOT_FOUND read fenced it.
        if (pending && !pending->started && pending->outcome == Pending::Outcome::running) {
            if (pending->permit.permit_id > refused_through &&
                ayni_control_start(gate, &pending->permit, now(), &live) == AYNI_OK) {
                accept_permit();
            } else {
                pending->outcome = Pending::Outcome::cancelled;
            }
        }
        const auto evaluated = ayni_control_check(gate, now(), &live);
        if (evaluated == AYNI_INVALID_ARGUMENT) {
            return evaluated;
        }
        ayni_control_info_v1 state{};
        state.struct_size = sizeof(state);
        const auto read = ayni_control_get_info(gate, &state);
        if (read != AYNI_OK) {
            return read;
        }
        if (state.phase == AYNI_CONTROL_STOPPING &&
            config.stop_confirmation == StopConfirmation::automatic) {
            const auto measured = measure_stop();
            if (measured != AYNI_OK) {
                return measured;
            }
        }
        if (!pending) {
            return AYNI_OK;
        }
        if (pending->outcome == Pending::Outcome::running) {
            if (state.phase != AYNI_CONTROL_ACTIVE || sample() != AYNI_OK) {
                pending->outcome = Pending::Outcome::cancelled;
            } else if (--pending->remaining == 0) {
                pending->outcome = Pending::Outcome::success;
                target = AYNI_TARGET_SATISFIED;
            }
        }
        if (pending->outcome == Pending::Outcome::running) {
            return AYNI_OK;
        }
        const bool success = pending->outcome == Pending::Outcome::success;
        const ayni_provider_result_v1 result{
            sizeof(result),
            success ? AYNI_CALL_COMPLETED : AYNI_CALL_CANCELLED,
            pending->attempt,
            0,
            AYNI_OK,
            AYNI_REASON_NONE,
            pending->produced ? (success ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_PARTIAL)
                              : AYNI_CALL_EFFECT_NONE,
            0,
            success ? text("turn complete") : ayni_string_v1{},
            0,
            0,
            nullptr};
        const auto posted = ayni_provider_post(pending->completion, &result);
        if (posted == AYNI_OK) {
            pending.reset();
        }
        return posted;
    }
};
Executor::Executor() = default;
Executor::~Executor() {
    if (close() != AYNI_OK) {
        static_cast<void>(impl_.release());
    }
}
ayni_status_t Executor::configure(const Config &config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if (config.now_us == nullptr || config.executor_boot_epoch == 0 ||
        config.ticks_to_complete == 0 || config.call_timeout_ms == 0 || config.limits.count == 0 ||
        config.limits.count > AYNI_CONTROL_FIELD_COUNT_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto value = std::unique_ptr<Impl>(new (std::nothrow) Impl{});
    if (!value) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    size_t alignment = 0;
    const auto status = ayni_control_memory_requirements(&value->memory_bytes, &alignment);
    if (status != AYNI_OK || alignment > alignof(uint64_t)) {
        return AYNI_INTERNAL;
    }
    value->memory.reset(new (
        std::nothrow) uint64_t[(value->memory_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t)]);
    if (!value->memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    value->config = config;
    value->describe();
    impl_ = std::move(value);
    return AYNI_OK;
}
ayni_status_t Executor::describe_provider(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = impl_->provider;
    return AYNI_OK;
}
ayni_status_t Executor::describe_executor(ayni_executor_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = impl_->executor;
    return AYNI_OK;
}
ayni_status_t Executor::advance() {
    return impl_ ? impl_->advance() : AYNI_INVALID_STATE;
}
ayni_status_t Executor::confirm_stopped() {
    return impl_ ? impl_->measure_stop() : AYNI_INVALID_STATE;
}
ayni_status_t Executor::answer() {
    return impl_ ? impl_->answer_held() : AYNI_INVALID_STATE;
}
void Executor::drop_requests() {
    if (impl_) {
        impl_->held_count = 0;
    }
}
uint32_t Executor::held(ayni_executor_request_t kind) const {
    uint32_t count = 0;
    for (uint32_t at = 0; impl_ && at < impl_->held_count; ++at) {
        count += impl_->held[at].kind == kind ? 1U : 0U;
    }
    return count;
}
ayni_status_t Executor::info(ayni_control_info_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    if (out.struct_size != sizeof(out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (impl_->gate != nullptr) {
        return ayni_control_get_info(impl_->gate, &out);
    }
    out = {};
    out.struct_size = sizeof(out);
    out.phase = AYNI_CONTROL_IDLE;
    return AYNI_OK;
}
uint32_t Executor::pending() const {
    return impl_ && impl_->pending ? 1U : 0U;
}
uint64_t Executor::samples() const {
    return impl_ ? impl_->sample_count : 0;
}
uint64_t Executor::renewals() const {
    return impl_ ? impl_->renewal_count : 0;
}
uint64_t Executor::submissions() const {
    return impl_ ? impl_->submit_count : 0;
}
uint64_t Executor::logical_call() const {
    return impl_ ? impl_->last_call : 0;
}
ayni_status_t Executor::close() {
    if (!impl_) {
        return AYNI_OK;
    }
    if (impl_->pending) {
        return AYNI_BUSY;
    }
    if (impl_->gate != nullptr) {
        const auto closed = ayni_control_destroy(impl_->gate);
        if (closed != AYNI_OK) {
            return closed;
        }
    }
    impl_.reset();
    return AYNI_OK;
}
} // namespace ayni::providers::mock_execution
