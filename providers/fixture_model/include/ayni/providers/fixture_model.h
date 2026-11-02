// Small deterministic model fixture. No network, parser dependency or heap allocation.
#ifndef AYNI_PROVIDERS_FIXTURE_MODEL_H
#define AYNI_PROVIDERS_FIXTURE_MODEL_H
#include <array>
#include <ayni/bodies.h>
#include <ayni/calls.h>
#include <optional>
namespace ayni::providers::fixture_model {
enum class Variant { a, b };           // Two distinct provider/endpoint identities; one capability.
enum class Service { tools, summary }; // Summary returns fixed fixture text, never a model claim.
// The longest prompt the fixture reads; a longer one is refused at admission.
inline constexpr uint32_t prompt_bytes_max = 32768;
inline constexpr uint32_t call_result_bytes = 1024;
// Canonical input bytes of the model Call: the whole inline prompt, or only its reference when
// prompts come from a body store (ADR-0096). The runtime sizes the Kernel's Call strides from it.
uint32_t call_input_bytes(bool bodies);
// Owner-thread only, no reentry. All members and the clock context outlive the
// registered Kernel; accepted requests retain only copied metadata and completion.
// Destruction requires pending()==false, after terminal posts and Kernel destroy.
class Provider {
  public:
    // With bodies, the prompt arrives only by reference and is read and verified at submit;
    // the store is borrowed through Kernel destroy.
    Provider(Variant, Service, void *clock_context, uint64_t (*now_us)(void *),
             const ayni_body_store_v1 *bodies = nullptr);
    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    // Descriptor views borrow this object through Kernel destroy. Null clock is invalid.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    // Exactly one terminal post per accepted request. Failed post retains the borrow.
    // A genuine latest user "sum" proposes software.sum(512); after a tool response
    // it returns final fixture text. "many sums" repeats eight sequential software
    // sums, for the normal soft-limit/owner-resume case. Context/system text never selects a
    // command.
    ayni_status_t advance();
    bool pending() const { return pending_.has_value(); }
    uint64_t submissions() const {
        return submissions_;
    } // Accepted borrows, including later cancellation.
    uint64_t executions() const { return executions_; } // Successful noncancelled model outputs.

  private:
    enum class Answer { sum, final, cancelled };
    struct Pending {
        Answer answer; // Copied choice, never a borrowed message view.
        ayni_attempt_ref_v1 attempt;
        uint64_t logical_call;                  // Stable protocol identity, positive.
        uint64_t deadline_us;                   // Kernel monotonic domain, positive.
        ayni_provider_completion_t *completion; // Borrow until successful terminal post.
    };
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
    Variant variant_;
    Service service_;
    void *clock_context_;        // Borrowed through destruction; null permitted by clock.
    uint64_t (*now_us_)(void *); // Required nonblocking owner clock.
    const ayni_body_store_v1 *bodies_;
    std::array<char, prompt_bytes_max> prompt_{}; // A referenced prompt, only during submit.
    ayni_call_resource_v1 resource_{};
    std::array<ayni_schema_node_v1, 2> input_{};
    ayni_schema_node_v1 result_{};
    ayni_business_contract_v1 business_{};
    ayni_call_capability_v1 capability_{};
    ayni_provider_v1 descriptor_{};
    std::optional<Pending> pending_; // One accepted borrow, empty is quiescent.
    uint64_t executions_ = 0;
    uint64_t submissions_ =
        0; // Owner-only acceptance count since construction, zero before submit.
};
} // namespace ayni::providers::fixture_model
#endif
