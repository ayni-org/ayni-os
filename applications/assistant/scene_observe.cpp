#include "policy_support.h"
#include <array>
#include <limits>
namespace ayni::applications::assistant::detail {
namespace {
constexpr uint32_t bits_per_byte = 8;
constexpr uint32_t state_fields = 8, field_bytes = 8, state_bytes = state_fields * field_bytes;
constexpr uint64_t state_version = 1, watching = 0, analyzing = 1, spotcheck_seconds = 300;
enum Field : uint32_t { version, phase, boot, generation, sequence, value, utc, round };
using State = std::array<uint64_t, state_fields>;
ayni_status_t load(const ayni_task_snapshot_v1 &s, State &state) {
    if (!s.checkpoint.state.size) {
        state[version] = state_version;
        return AYNI_OK;
    }
    if (s.checkpoint.state.size != state_bytes) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    for (uint32_t field = 0; field < state_fields; ++field) {
        for (uint32_t byte = 0; byte < field_bytes; ++byte) {
            state[field] |=
                static_cast<uint64_t>(s.checkpoint.state.data[field * field_bytes + byte])
                << (byte * bits_per_byte);
        }
    }
    return state[version] == state_version && state[phase] <= analyzing ? AYNI_OK
                                                                        : AYNI_UNSUPPORTED_VERSION;
}
ayni_status_t save(const State &state, ayni_policy_output_v1 &o) {
    if (!o.checkpoint || o.checkpoint_capacity < state_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t field = 0; field < state_fields; ++field) {
        for (uint32_t byte = 0; byte < field_bytes; ++byte) {
            o.checkpoint[field * field_bytes + byte] =
                static_cast<uint8_t>(state[field] >> (byte * bits_per_byte));
        }
    }
    o.checkpoint_size = state_bytes;
    return AYNI_OK;
}
uint64_t trusted_utc(const PolicyConfig &c) {
    ayni_utc_sample_v1 time{sizeof(time), AYNI_UTC_UNAVAILABLE, 0};
    if (c.utc_now) {
        c.utc_now(c.clock_context, &time);
    }
    return time.struct_size == sizeof(time) && time.trust == AYNI_UTC_TRUSTED && time.utc_s >= 0 &&
                   time.utc_s <= AYNI_CALENDAR_SECONDS_MAX
               ? static_cast<uint64_t>(time.utc_s)
               : 0;
}
} // namespace
// Only current usable Task-owned samples drive analysis. Stable round keys bound
// model calls via Core's existing Task limit; WAIT retains collection without polling.
ayni_status_t observe(const PolicyConfig &c, const ayni_task_snapshot_v1 &s,
                      ayni_policy_output_v1 &o) {
    if (!s.subscription || !s.subscription_observation) {
        return AYNI_UNSUPPORTED;
    }
    const auto watch_key = text("observe.watch.v1");
    ayni_subscription_info_v1 subscription{};
    subscription.struct_size = sizeof(subscription);
    auto status = s.subscription(s.call_context, watch_key, &subscription);
    if (status == AYNI_NOT_FOUND) {
        return request(o, "observe", watch_key, {}, empty_scope());
    }
    if (status != AYNI_OK) {
        return status;
    }
    State state{};
    status = load(s, state);
    if (status != AYNI_OK) {
        return status;
    }
    if (state[phase] == analyzing) {
        const auto key = "observe.analysis." + std::to_string(state[round]);
        ayni_task_call_result_v1 call{};
        status = result(s, text(key), call);
        const bool cancelled = confirmed_cancel(call);
        if (status != AYNI_OK && !cancelled) {
            return status;
        }
        state[phase] = watching;
        status = save(state, o);
        // A confirmed cancelled model round is retained/charged, not retried.
        // Reopening collection provides a new generation and a new analysis key.
        const auto done = std::string(cancelled ? "observe.aborted." : "observe.done.") +
                          std::to_string(state[round]);
        return status == AYNI_OK ? report(s, o, AYNI_POLICY_REPORT_PROGRESS, text(done)) : status;
    }
    ayni_observation_info_v1 sample{};
    sample.struct_size = sizeof(sample);
    status = s.subscription_observation(s.call_context, watch_key, &sample);
    if (status != AYNI_OK && status != AYNI_NOT_FOUND) {
        return status;
    }
    const auto now = trusted_utc(c);
    const bool due =
        now && (!state[utc] || (now >= state[utc] && now - state[utc] >= spotcheck_seconds));
    if (status == AYNI_OK && sample.usable &&
        (due || sample.reference.boot_epoch != state[boot] ||
         sample.reference.generation != state[generation] ||
         static_cast<uint64_t>(sample.value) != state[value])) {
        if (state[round] == std::numeric_limits<uint64_t>::max()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        ++state[round];
        state[phase] = analyzing;
        state[boot] = sample.reference.boot_epoch;
        state[generation] = sample.reference.generation;
        state[sequence] = sample.reference.sequence;
        state[value] = static_cast<uint64_t>(sample.value);
        state[utc] = now;
        status = save(state, o);
        if (status != AYNI_OK) {
            return status;
        }
        const auto key = "observe.analysis." + std::to_string(state[round]);
        const auto prompt =
            model_body(
                "分析 Mock 场景的标量观测（非真实图像），简短描述，不推断真实设备或执行动作。") +
            "值=" + std::to_string(sample.value) +
            "，序列=" + std::to_string(sample.reference.sequence) + "。";
        return request(o, "summary", text(key), text(prompt), c.model_scope, true);
    }
    // Subscription controls use the request stager as well as the public intent;
    // the stager captures the current generation/frontier for commit validation.
    if (!o.request_call) {
        return AYNI_UNSUPPORTED;
    }
    ayni_call_request_v1 wait{};
    wait.struct_size = sizeof(wait);
    wait.kind = AYNI_POLICY_WAIT_SUBSCRIPTION;
    wait.step_key = watch_key;
    return o.request_call(o.call_context, &wait);
}
} // namespace ayni::applications::assistant::detail
