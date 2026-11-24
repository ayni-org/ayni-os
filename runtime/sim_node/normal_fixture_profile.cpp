#include "normal_fixture_profile.h"
#include "fixture_call_barrier.h"
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/host_authority.h>
#include <ayni/memory.h>
#include <ayni/resources.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/runtime/common/memory_capability.h>
#include <cstring>
namespace ayni::runtime::fixtures {
namespace {
constexpr uint32_t text_limit = 512;
constexpr uint32_t request_timeout_ms = 10000;
providers::mock::Config health_config() {
    return {providers::mock::Variant::a, providers::mock::Service::health, 1, text_limit, {}};
}
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size &&
           (!a.size || (a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0));
}
ayni_status_t register_capability(ayni_kernel_t *kernel, const ayni_provider_v1 &offer) {
    const ayni_capability_desc_v1 cap{sizeof(cap), 1, offer.capabilities->capability_id,
                                      0,           0, nullptr};
    auto status = ayni_register_capability(kernel, &cap);
    return status == AYNI_OK ? ayni_register_call_capability(kernel, offer.capabilities) : status;
}
ayni_status_t register_offer(ayni_kernel_t *kernel, const ayni_provider_v1 &offer,
                             const char *binding) {
    auto status = ayni_register_provider(kernel, &offer);
    if (status == AYNI_OK && binding) {
        const ayni_binding_v1 value{sizeof(value),
                                    1,
                                    text(binding),
                                    offer.capabilities->capability_id,
                                    1,
                                    1,
                                    offer.provider_id,
                                    offer.endpoint_id,
                                    offer.destination_id,
                                    0,
                                    0,
                                    nullptr};
        status = ayni_register_binding(kernel, &value);
    }
    return status;
}
// A trusted fixture domain using the public scope builder's four fields.
// The normal profile has no external egress destination or credential.
ayni_status_t register_memory(ayni_kernel_t *kernel) {
    auto fields = memory_access_fields();
    const ayni_capability_desc_v1 capability{
        sizeof(capability), 1, text("memory.access"), static_cast<uint32_t>(fields.size()), 0,
        fields.data()};
    return ayni_register_capability(kernel, &capability);
}
} // namespace
Counter::Counter(const char *capability, Effect effect) {
    const bool notification = effect == Effect::notification;
    resource_ = {text(notification ? "notification.slot" : "counter.slot"), 1, 0};
    capability_ = initialized<ayni_call_capability_v1>();
    capability_.version = 1;
    capability_.capability_id = text(capability);
    capability_.contract = {sizeof(ayni_call_contract_v1),
                            AYNI_CALL_ACTION,
                            AYNI_CALL_TOOL,
                            notification ? AYNI_CALL_EFFECT_NOTIFICATION : 0U,
                            AYNI_CALL_NOT_IDEMPOTENT,
                            AYNI_CALL_QUERY_NONE,
                            AYNI_CALL_CANCEL_REQUEST,
                            0,
                            request_timeout_ms,
                            AYNI_CALL_INPUT_VALUE,
                            AYNI_CALL_RESULT_TEXT,
                            input_bytes(),
                            0,
                            1,
                            &resource_,
                            nullptr,
                            AYNI_CALL_DURABILITY_DURABLE,
                            0};
    input_schema_.kind = result_schema_.kind = AYNI_VALUE_TEXT;
    input_schema_.subtree_end = result_schema_.subtree_end = 1;
    input_schema_.max_count = text_limit;
    business_ = {{&input_schema_, 1}, {&result_schema_, 1}, 0, 0, nullptr};
    capability_.business = &business_;
}
uint32_t Counter::input_bytes() {
    return text_value_bytes(text_limit);
}
ayni_provider_v1 Counter::describe() {
    auto out = initialized<ayni_provider_v1>();
    out.version = 1;
    out.provider_id = out.endpoint_id = capability_.capability_id;
    out.ctx = this;
    out.capability_count = 1;
    out.capabilities = &capability_;
    out.submit = submit;
    out.cancel = cancel;
    return out;
}
ayni_submit_result_t Counter::submit(void *ctx, const ayni_provider_request_v1 *request,
                                     ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Counter *>(ctx);
    if (!request || !completion || request->struct_size != sizeof(*request) ||
        request->capability_version != 1 ||
        !same(request->capability_id, self.capability_.capability_id) ||
        !same(request->endpoint_id, self.capability_.capability_id)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ValueReader input;
    ayni_string_v1 body{};
    if (input.read(request->value) != AYNI_OK || !ValueReader::text(input.root(), body) ||
        body.size > text_limit) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    for (auto &slot : self.slots_) {
        if (slot.phase == Phase::empty) {
            slot = {Phase::queued, request->logical_call_id, request->attempt, completion};
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t Counter::cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Counter *>(ctx);
    for (auto &slot : self.slots_) {
        if (slot.phase != Phase::empty && slot.attempt.boot_epoch == attempt.boot_epoch &&
            slot.attempt.request_id == attempt.request_id) {
            if (slot.phase == Phase::queued) {
                slot.phase = Phase::cancelled;
            }
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_status_t Counter::advance(ayni_kernel_t *kernel, uint64_t &barrier_checks) {
    for (auto &slot : slots_) {
        if (slot.phase == Phase::empty) {
            continue;
        }
        if (slot.phase == Phase::queued) {
            const auto status = check_barrier(kernel, slot.call);
            if (status != AYNI_OK) {
                return status;
            }
            ++barrier_checks;
            ++executions_;
            slot.phase = Phase::terminal; // Receipt backpressure cannot execute the effect again.
        }
        const bool cancelled = slot.phase == Phase::cancelled;
        auto result = initialized<ayni_provider_result_v1>();
        result.outcome = cancelled ? AYNI_CALL_CANCELLED : AYNI_CALL_COMPLETED;
        result.attempt = slot.attempt;
        result.status = AYNI_OK;
        result.effect = cancelled ? AYNI_CALL_EFFECT_NONE : AYNI_CALL_EFFECT_COMPLETE;
        const auto status = ayni_provider_post(slot.completion, &result);
        if (status == AYNI_BUSY) {
            continue;
        }
        if (status != AYNI_OK) {
            return status;
        }
        slot = {};
    }
    return AYNI_OK;
}
bool Counter::pending() const {
    for (const auto &slot : slots_) {
        if (slot.phase != Phase::empty) {
            return true;
        }
    }
    return false;
}
const char *Profile::capability(const char *alias) {
    if (std::strcmp(alias, "model") == 0) {
        return "mock.text";
    }
    if (std::strcmp(alias, "increment") == 0) {
        return "test.increment";
    }
    if (std::strcmp(alias, "notify") == 0) {
        return "notify.present";
    }
    if (std::strcmp(alias, "observe") == 0) {
        return "mock.observe";
    }
    if (std::strcmp(alias, "health") == 0) {
        return "mock.health";
    }
    return "";
}
void Profile::fit_call_bytes(ayni_config_v1 &config) {
    const auto health = health_config();
    runtime::fit_call_bytes(
        config,
        {{ModelProbe::call_input_bytes(), ModelProbe::call_result_bytes},
         {Counter::input_bytes(), 0},
         {providers::mock::subscription_input_bytes(), providers::mock::subscription_result_bytes},
         {providers::mock::call_input_bytes(health), providers::mock::call_result_bytes(health)}});
}
ayni_status_t Profile::assemble(ayni_kernel_t *kernel, Clock clock, const Config &config,
                                Provider provider, Strategy strategy) {
    kernel_ = kernel;
    clock_ = clock;
    auto status = model_a_.configure(providers::mock::Variant::a, config.model_delay_us, clock);
    if (status == AYNI_OK) {
        status = model_b_.configure(providers::mock::Variant::b, config.model_delay_us, clock);
    }
    providers::mock::SubscriptionConfig camera;
    camera.clock_context = clock.context;
    camera.now_us = clock.now_us;
    // Keep the reusable Mock's normal 1 s cadence / 2 s TTL. Emitting every
    // owner tick would turn this integration case into an ingress flood test.
    if (status == AYNI_OK) {
        status = camera_.configure(camera);
    }
    if (status == AYNI_OK) {
        ayni_provider_v1 offer{};
        status = camera_.describe(offer);
        if (status == AYNI_OK) {
            camera_offer_ = offer;
        }
    }
    if (status == AYNI_OK) {
        status = health_.configure(health_config());
    }
    if (status == AYNI_OK) {
        ayni_provider_v1 offer{};
        status = health_.describe(offer);
        if (status == AYNI_OK) {
            health_offer_ = offer;
        }
    }
    return status == AYNI_OK ? register_services(provider, strategy) : status;
}
ayni_status_t Profile::register_services(Provider selected, Strategy revision) {
    const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, 1, 1, 0};
    auto status = ayni_register_principal(kernel_, &owner);
    for (uint64_t id : {UINT64_C(1), UINT64_C(2)}) {
        const ayni_agent_desc_v1 agent{sizeof(agent), 0, id};
        if (status == AYNI_OK) {
            status = ayni_register_agent(kernel_, &agent);
        }
    }
    for (const char *id :
         {"model.slot", "counter.slot", "notification.slot", "camera.slot", "device.slot"}) {
        const ayni_resource_desc_v1 resource{sizeof(resource),
                                             AYNI_RESOURCE_SHARED,
                                             text(id),
                                             std::strcmp(id, "camera.slot") == 0 ? 2U : 1U,
                                             0,
                                             0,
                                             0,
                                             nullptr,
                                             {}};
        if (status == AYNI_OK) {
            status = ayni_register_resource(kernel_, &resource);
        }
        if (status == AYNI_OK) {
            status = ayni_set_resource_health(kernel_, resource.resource_id, AYNI_RESOURCE_ONLINE);
        }
    }
    auto a = model_a_.describe(), b = model_b_.describe();
    const auto increment = increment_.describe(), notify = notification_.describe();
    for (const auto &offer : {a, increment, notify, *health_offer_, *camera_offer_}) {
        if (status == AYNI_OK) {
            status = register_capability(kernel_, offer);
        }
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, a, selected == Provider::a ? "model" : nullptr);
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, b, selected == Provider::b ? "model" : nullptr);
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, increment, "increment");
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, notify, "notify");
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, *health_offer_, "health");
    }
    if (status == AYNI_OK) {
        status = register_offer(kernel_, *camera_offer_, "observe");
    }
    auto source = initialized<ayni_observation_source_v1>();
    if (status == AYNI_OK) {
        status = camera_.describe_source(source);
    }
    if (status == AYNI_OK) {
        status = ayni_register_observation_source(kernel_, &source);
    }
    if (status == AYNI_OK) {
        status = register_memory(kernel_);
    }
    if (status == AYNI_OK) {
        status = policy_.register_all(kernel_, revision);
    }
    applications::assistant::PolicyConfig config;
    config.model_capability = text("mock.text");
    config.present_capability = text("notify.present");
    config.health_capability = text("mock.health");
    config.observe_capability = text("mock.observe");
    config.clock_context = clock_.context;
    config.utc_now = clock_.utc_now;
    return status == AYNI_OK ? assistant_.register_all(kernel_, config) : status;
}
ayni_status_t Profile::advance() {
    auto status = model_a_.advance(kernel_, barrier_checks_);
    if (status == AYNI_OK) {
        status = model_b_.advance(kernel_, barrier_checks_);
    }
    if (status == AYNI_OK) {
        status = increment_.advance(kernel_, barrier_checks_);
    }
    if (status == AYNI_OK) {
        status = notification_.advance(kernel_, barrier_checks_);
    }
    if (status == AYNI_OK && camera_offer_) {
        status = camera_.advance();
    }
    if (status == AYNI_OK && health_offer_) {
        status = health_.advance();
    }
    return status == AYNI_BUSY ? AYNI_OK : status;
}
ayni_status_t Profile::close() {
    if (increment_.pending() || notification_.pending() || camera_.pending()) {
        return AYNI_BUSY;
    }
    auto status = model_a_.close();
    if (status == AYNI_OK) {
        status = model_b_.close();
    }
    if (status == AYNI_OK && camera_offer_) {
        status = camera_.close();
        if (status == AYNI_OK) {
            camera_offer_.reset();
        }
    }
    if (status == AYNI_OK && health_offer_) {
        status = health_.close();
        if (status == AYNI_OK) {
            health_offer_.reset();
        }
    }
    return status;
}
Totals Profile::totals() const {
    return {model_a_.executions(),      model_b_.executions(), increment_.executions(),
            notification_.executions(), barrier_checks_,       health_.executions()};
}
} // namespace ayni::runtime::fixtures
