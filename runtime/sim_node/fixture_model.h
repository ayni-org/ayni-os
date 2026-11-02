// Shared delayed Mock adapter for normal and full deterministic profiles.
#ifndef AYNI_RUNTIME_FIXTURE_MODEL_H
#define AYNI_RUNTIME_FIXTURE_MODEL_H
#include <ayni/providers/mock.h>
#include <ayni/runtime/common/clock.h>
#include <optional>
namespace ayni::runtime::fixtures {
// Real reusable Mock plus a fake-time gate; the probe owns only dispatch diagnostics.
class ModelProbe {
  public:
    // With bodies, a model request may name its body by reference (ADR-0096).
    ayni_status_t configure(providers::mock::Variant, uint64_t delay_us, Clock,
                            std::optional<uint32_t> timeout_ms = std::nullopt,
                            providers::mock::Service = providers::mock::Service::text,
                            const ayni_body_store_v1 *bodies = nullptr);
    // Contract maxima of the declared model Call, for the Kernel's Call strides.
    static uint32_t call_input_bytes();
    static constexpr uint32_t call_result_bytes = 512;
    ayni_provider_v1 describe();
    ayni_status_t advance(ayni_kernel_t *, uint64_t &barrier_checks);
    ayni_status_t close();
    ayni_status_t set_delay(uint64_t delay_us);
    enum class Cancellation { confirm, late_success };
    ayni_status_t set_cancellation(Cancellation mode);
    void abandon_kernel(); // Only when this simulated Kernel can never run again.
    uint32_t pending() const { return provider_.pending(); }
    uint64_t next_due_us() const { return dispatch_ ? dispatch_->due_us : UINT64_MAX; }
    uint64_t executions() const { return provider_.executions(); }
    // Value-only packet identity survives a cut; the target always belongs to
    // a current live delivery. Never expose an old arena pointer to a scenario.
    std::optional<ayni_attempt_ref_v1> attempt() const;
    uint64_t current_call() const { return dispatch_ ? dispatch_->call : 0; }
    ayni_status_t post_attempt(ayni_attempt_ref_v1);

  private:
    providers::mock::Provider provider_;
    std::optional<ayni_provider_v1> original_;
    ayni_call_capability_v1 declaration_{}; // Adapter-owned contract may tighten the Mock deadline.
    Clock clock_{};
    uint64_t delay_us_ = 0;
    Cancellation cancellation_ = Cancellation::confirm;
    enum class DispatchPhase { queued, checked, cancelled };
    struct Dispatch {
        uint64_t call;
        uint64_t due_us;
        DispatchPhase phase;
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *target;
    };
    std::optional<Dispatch> dispatch_;
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
};
} // namespace ayni::runtime::fixtures
#endif
