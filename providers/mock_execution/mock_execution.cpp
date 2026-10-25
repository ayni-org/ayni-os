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
    struct Pending {
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        uint32_t remaining;
        enum class Outcome { running, success, cancelled } outcome = Outcome::running;
        bool produced = false; // Whether this operation emitted a permitted sample.
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
                       1, &resource}};
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
                    challenge,
                    renew,
                    stop,
                    status};
    }
    // The trusted Core supplies its new boot at the first challenge. This gate is
    // never recreated or rebound after it has been configured for that generation.
    static ayni_status_t challenge(void *context, const ayni_permit_epochs_v1 *expected,
                                   ayni_executor_nonce_v1 *out) {
        auto &self = *static_cast<Impl *>(context);
        if (expected == nullptr || out == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (expected->executor_boot_epoch != self.config.executor_boot_epoch ||
            expected->safety_epoch != 1 || expected->calibration_version != 1) {
            return AYNI_DENIED;
        }
        if (self.gate == nullptr) {
            const ayni_control_config_v1 config{sizeof(config),
                                                AYNI_ABI_MAJOR,
                                                AYNI_ABI_MINOR,
                                                1,
                                                domain,
                                                *expected,
                                                duration_max_us,
                                                duration_max_us,
                                                sample_age_max_us,
                                                0,
                                                self.config.limits};
            const auto created = ayni_control_create(&config, self.now(), self.memory.get(),
                                                     self.memory_bytes, &self.gate);
            if (created != AYNI_OK) {
                return created;
            }
            self.live = {*expected, 1, 0};
        } else if (!same(*expected, self.live.epochs)) {
            return AYNI_STALE;
        }
        return ayni_control_nonce(self.gate, self.now(), out);
    }
    static ayni_status_t renew(void *context, const ayni_permit_renewal_v1 *renewal) {
        auto &self = *static_cast<Impl *>(context);
        if (self.gate == nullptr) {
            return AYNI_NOT_READY;
        }
        const auto status = ayni_control_renew(self.gate, renewal, self.now(), &self.live);
        if (status == AYNI_OK) {
            ++self.renewal_count;
        }
        return status;
    }
    static ayni_status_t stop(void *context, uint64_t permit_id) {
        auto &self = *static_cast<Impl *>(context);
        return self.gate == nullptr ? AYNI_NOT_FOUND
                                    : ayni_control_stop(self.gate, permit_id, self.now());
    }
    static ayni_status_t status(void *context, uint64_t permit_id, ayni_executor_status_v1 *out) {
        auto &self = *static_cast<Impl *>(context);
        if (out == nullptr || out->struct_size != sizeof(*out)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (self.gate == nullptr || permit_id > self.permit.permit_id) {
            return AYNI_NOT_FOUND;
        }
        if (permit_id != self.permit.permit_id) {
            return AYNI_STALE;
        }
        // check updates expiry/interlocks; its denial is represented by the exact copied state.
        const auto evaluated = ayni_control_check(self.gate, self.now(), &self.live);
        if (evaluated == AYNI_INVALID_ARGUMENT) {
            return evaluated;
        }
        out->target = self.target;
        out->control.struct_size = sizeof(out->control);
        return ayni_control_get_info(self.gate, &out->control);
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
            self.gate == nullptr || self.pending) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        if (ayni_control_start(self.gate, request->execution_permit, self.now(), &self.live) !=
            AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.permit = *request->execution_permit;
        self.pending = Pending{request->attempt, completion, self.config.ticks_to_complete};
        self.last_call = request->logical_call_id;
        self.sequence = 0;
        self.target = AYNI_TARGET_UNVERIFIED;
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
        const auto status = stop(context, self.permit.permit_id);
        return status == AYNI_OK ? AYNI_SUBMIT_ACCEPTED : AYNI_SUBMIT_ACCEPT_UNKNOWN;
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
            success ? text("turn complete") : ayni_string_v1{}};
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
