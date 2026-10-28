// Concrete, single-owner fixture deployment; producers progress outside step.
#ifndef AYNI_RUNTIME_NORMAL_FIXTURE_PROFILE_H
#define AYNI_RUNTIME_NORMAL_FIXTURE_PROFILE_H
#include "fixture_model.h"
#include <array>
#include <ayni/applications/assistant.h>
#include <ayni/applications/normal_fixtures.h>
#include <ayni/providers/mock.h>
#include <ayni/providers/mock/subscription.h>
#include <ayni/runtime/common/clock.h>
#include <ayni/runtime/sim_node/fixtures.h>
#include <optional>
namespace ayni::runtime::fixtures {
struct Totals {
    uint64_t model_a = 0, model_b = 0, increments = 0, presentations = 0, barrier_checks = 0,
             health = 0;
};
// Deterministic fixture sink. Acceptance queues; only advance performs the effect.
// Slots retain their completion until terminal post succeeds. Callbacks allocate nothing.
class Counter {
  public:
    enum class Effect { increment, notification };
    Counter(const char *capability, Effect);
    ayni_provider_v1 describe();
    ayni_status_t advance(ayni_kernel_t *, uint64_t &barrier_checks);
    uint64_t executions() const { return executions_; }
    bool pending() const;

  private:
    enum class Phase { empty, queued, cancelled, terminal };
    struct Slot {
        Phase phase = Phase::empty;
        uint64_t call = 0;
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
    };
    std::array<Slot, 4> slots_{};
    ayni_call_resource_v1 resource_{};
    ayni_call_capability_v1 capability_{};
    uint64_t executions_ = 0;
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
};
class Profile {
  public:
    ayni_status_t assemble(ayni_kernel_t *, Clock, const Config &, Provider, Strategy);
    ayni_status_t advance();
    ayni_status_t close(); // Only after successful Kernel destruction.
    Totals totals() const;
    static const char *capability(const char *alias);

  private:
    ayni_kernel_t *kernel_ = nullptr;
    Clock clock_{};
    ModelProbe model_a_, model_b_;
    Counter increment_{"test.increment", Counter::Effect::increment};
    Counter notification_{"notify.present", Counter::Effect::notification};
    providers::mock::SubscriptionProvider camera_;
    providers::mock::Provider health_;
    std::optional<ayni_provider_v1> camera_offer_, health_offer_;
    applications::normal_fixtures::Policy policy_;
    applications::assistant::Goals assistant_;
    uint64_t barrier_checks_ = 0;
    ayni_status_t register_services(Provider, Strategy);
};
} // namespace ayni::runtime::fixtures
#endif
