// A simulated tactile button adapter using only the public subscription ABI.
#include "support.h"
#include <ayni/observations.h>
#include <optional>
namespace {
using namespace ayni::examples;
constexpr uint32_t version = 1, lease_ms = 30000;
constexpr uint64_t sample_ttl_us = 1000000;
struct Button {
    enum class Phase { starting, active, stopping };
    struct Delivery {
        ayni_subscription_open_v1 opening;
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        Phase phase = Phase::starting;
        uint64_t lease_sequence = 0;  // 0 before start ACK, then strictly increasing.
        uint64_t sample_sequence = 0; // 0 before the single simulated press was accepted.
        std::optional<ayni_subscription_renew_v1> renewal;
    };
    std::optional<Delivery> delivery;
    ayni_call_resource_v1 resource{text("button.slot"), 1, 0};
    ayni_subscription_contract_v1 collection{sizeof(collection), version, text("example.button"),
                                             lease_ms, 0};
    ayni_call_capability_v1 capability{};
    ayni_schema_node_v1 input_schema{}, result_schema{};
    ayni_business_contract_v1 business{};
    uint64_t now = 0;
    uint32_t openings = 0, samples = 0, stops = 0;
    static bool same(ayni_subscription_ref_v1 a, ayni_subscription_ref_v1 b) {
        return a.subscription_id == b.subscription_id && a.boot_epoch == b.boot_epoch &&
               a.generation == b.generation;
    }
    static ayni_submit_result_t submit(void *ctx, const ayni_provider_request_v1 *r,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Button *>(ctx);
        if (!r || !r->subscription || !completion || self.delivery ||
            r->struct_size != sizeof(*r) ||
            r->subscription->struct_size != sizeof(ayni_subscription_open_v1) ||
            !r->subscription->sink || self.now >= r->subscription->valid_until_us) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.delivery =
            Delivery{*r->subscription, r->attempt, completion, Phase::starting, 0, 0, {}};
        ++self.openings;
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Button *>(ctx);
        if (!self.delivery || self.delivery->attempt.boot_epoch != attempt.boot_epoch ||
            self.delivery->attempt.request_id != attempt.request_id) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.delivery->phase = Phase::stopping;
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t renew(void *ctx, const ayni_subscription_renew_v1 *r) {
        auto &self = *static_cast<Button *>(ctx);
        if (!r || !self.delivery || r->struct_size != sizeof(*r) || r->reserved ||
            !same(r->subscription, self.delivery->opening.subscription) ||
            self.delivery->phase != Phase::active || self.delivery->renewal ||
            self.delivery->lease_sequence == UINT64_MAX ||
            r->sequence != self.delivery->lease_sequence + 1 ||
            self.now >= self.delivery->opening.valid_until_us ||
            r->valid_until_us <= self.delivery->opening.valid_until_us ||
            r->valid_until_us == UINT64_MAX) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.delivery->renewal = *r;
        self.delivery->opening.valid_until_us = r->valid_until_us;
        return AYNI_SUBMIT_ACCEPTED;
    }
    ayni_status_t register_all(ayni_kernel_t *kernel) {
        const ayni_resource_desc_v1 slot{
            sizeof(slot), AYNI_RESOURCE_EXCLUSIVE, text("button.slot"), 1, 0, 0, 0, nullptr, {}};
        auto status = ayni_register_resource(kernel, &slot);
        if (status == AYNI_OK) {
            status = ayni_set_resource_health(kernel, slot.resource_id, AYNI_RESOURCE_ONLINE);
        }
        const ayni_observation_source_v1 source{sizeof(source),
                                                version,
                                                text("example.button"),
                                                AYNI_OBSERVATION_BOOL,
                                                AYNI_OBSERVATION_MEASURED,
                                                text("bool"),
                                                sample_ttl_us};
        if (status == AYNI_OK) {
            status = ayni_register_observation_source(kernel, &source);
        }
        const ayni_capability_desc_v1 cap{sizeof(cap), version, text("example.button.observe"),
                                          0,           0,       nullptr};
        if (status == AYNI_OK) {
            status = ayni_register_capability(kernel, &cap);
        }
        capability = ayni::initialized<ayni_call_capability_v1>();
        capability.version = version;
        capability.capability_id = cap.capability_id;
        capability.contract = {sizeof(ayni_call_contract_v1),
                               AYNI_CALL_SUBSCRIPTION,
                               AYNI_CALL_TOOL,
                               0,
                               AYNI_CALL_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_CONFIRMED,
                               0,
                               lease_ms,
                               AYNI_CALL_INPUT_VALUE,
                               AYNI_CALL_RESULT_TEXT,
                               24,
                               0,
                               1,
                               &resource,
                               &collection};
        input_schema.kind = result_schema.kind = AYNI_VALUE_TEXT;
        input_schema.subtree_end = result_schema.subtree_end = 1;
        business = {{&input_schema, 1}, {&result_schema, 1}, 0, 0, nullptr};
        capability.business = &business;
        if (status == AYNI_OK) {
            status = ayni_register_call_capability(kernel, &capability);
        }
        const ayni_provider_v1 provider{sizeof(provider),
                                        version,
                                        text("example.button.provider"),
                                        text("example.button.port"),
                                        {},
                                        this,
                                        1,
                                        0,
                                        &capability,
                                        submit,
                                        cancel,
                                        nullptr,
                                        renew};
        if (status == AYNI_OK) {
            status = ayni_register_provider(kernel, &provider);
        }
        const ayni_binding_v1 binding{sizeof(binding),
                                      version,
                                      text("button"),
                                      cap.capability_id,
                                      version,
                                      version,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        return status == AYNI_OK ? ayni_register_binding(kernel, &binding) : status;
    }
    ayni_status_t advance(uint64_t clock) {
        now = clock;
        if (!delivery) {
            return AYNI_OK;
        }
        auto &d = *delivery;
        if (now >= d.opening.valid_until_us) {
            d.phase = Phase::stopping;
        }
        if (d.phase == Phase::stopping) {
            auto result = ayni::initialized<ayni_provider_result_v1>();
            result.outcome = AYNI_CALL_CANCELLED;
            result.attempt = d.attempt;
            result.status = AYNI_OK;
            result.effect = d.sample_sequence ? AYNI_CALL_EFFECT_PARTIAL : AYNI_CALL_EFFECT_NONE;
            const auto status = ayni_provider_post(d.completion, &result);
            if (status == AYNI_OK) {
                delivery.reset();
                ++stops;
            }
            return status == AYNI_BUSY ? AYNI_OK : status;
        }
        if (d.phase == Phase::starting || d.renewal) {
            auto result = ayni::initialized<ayni_provider_result_v1>();
            result.outcome = d.phase == Phase::starting ? AYNI_CALL_SUBSCRIPTION_STARTED
                                                        : AYNI_CALL_SUBSCRIPTION_RENEWED;
            result.attempt = d.attempt;
            result.sequence = d.renewal ? d.renewal->sequence : 1;
            result.status = AYNI_OK;
            result.effect = AYNI_CALL_EFFECT_NONE;
            const auto status = ayni_provider_post(d.completion, &result);
            if (status == AYNI_OK) {
                d.phase = Phase::active;
                d.lease_sequence = result.sequence;
                d.renewal.reset();
            }
            return status == AYNI_BUSY ? AYNI_OK : status;
        }
        if (!d.sample_sequence) {
            const ayni_observation_v1 sample{sizeof(sample),
                                             version,
                                             text("example.button"),
                                             d.opening.subscription.generation,
                                             1,
                                             now,
                                             sample_ttl_us,
                                             AYNI_OBSERVATION_GOOD,
                                             0,
                                             1};
            const ayni_subscription_observation_v1 post{sizeof(post), 0, d.opening.subscription,
                                                        sample, nullptr};
            const auto status = ayni_subscription_post_observation(d.opening.sink, &post);
            if (status == AYNI_OK) {
                d.sample_sequence = 1;
                ++samples;
            } else if (status == AYNI_STALE) {
                d.phase = Phase::stopping;
            }
            // Start ACK may still be queued in Core; NOT_READY does not prove
            // collection was cancelled. Retain the opening until control/lease expiry.
            else if (status == AYNI_NOT_READY) {
                return AYNI_OK;
            } else if (status != AYNI_BUSY) {
                return status;
            }
        }
        return AYNI_OK;
    }
};
struct Example {
    Button button;
    static ayni_status_t requirements(void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
        const ayni_goal_requirement_v1 need{sizeof(need), version, text("example.button.observe"),
                                            empty_scope()};
        return sink->emit(sink->ctx, &need);
    }
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *s,
                                ayni_policy_output_v1 *out) {
        auto subscription = ayni::initialized<ayni_subscription_info_v1>();
        auto status = s->subscription(s->call_context, text("button.press"), &subscription);
        if (status == AYNI_NOT_FOUND) {
            return request(*out, AYNI_POLICY_REQUEST_CALL, "button.press", "button");
        }
        if (status != AYNI_OK) {
            return status;
        }
        if (subscription.phase == AYNI_SUBSCRIPTION_STOPPED) {
            return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
        }
        auto sample = ayni::initialized<ayni_observation_info_v1>();
        status = s->subscription_observation(s->call_context, text("button.press"), &sample);
        if (status == AYNI_OK && sample.usable && sample.value == 1) {
            return request(*out, AYNI_POLICY_CLOSE_SUBSCRIPTION, "button.press");
        }
        if (status != AYNI_OK && status != AYNI_NOT_FOUND) {
            return status;
        }
        return request(*out, AYNI_POLICY_WAIT_SUBSCRIPTION, "button.press");
    }
    static ayni_status_t configure(void *ctx, ayni_kernel_t *kernel) {
        const auto status = static_cast<Example *>(ctx)->button.register_all(kernel);
        return status == AYNI_OK ? install(kernel, "example.hardware", "example.button_rules", ctx,
                                           decide, requirements)
                                 : status;
    }
    static ayni_status_t advance(void *ctx, uint64_t now) {
        return static_cast<Example *>(ctx)->button.advance(now);
    }
    static ayni_status_t quiesce(void *ctx) {
        return static_cast<Example *>(ctx)->button.delivery ? AYNI_BUSY : AYNI_OK;
    }
};
} // namespace
int main() {
    Example example;
    ayni::runtime::examples::Fixture fixture;
    auto status = fixture.open({&example, Example::configure, Example::advance, Example::quiesce});
    if (status == AYNI_OK) {
        status = fixture.grant(text("example.button.observe"), empty_scope());
    }
    auto request = goal(fixture, "example.hardware");
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(request, id);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    if (status == AYNI_OK && (example.button.openings != 1 || example.button.samples != 1 ||
                              example.button.stops != 1)) {
        status = AYNI_INVALID_STATE;
    }
    return finish("new_hardware (simulated tactile button)", status, fixture);
}
