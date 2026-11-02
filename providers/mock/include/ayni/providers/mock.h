// Deterministic asynchronous text/action Provider for Native demos and E1/E3/E4.
// Uses only the public C ABI; it makes no hardware, network or persistence claim.
#ifndef AYNI_PROVIDERS_MOCK_H
#define AYNI_PROVIDERS_MOCK_H
#include <ayni/bodies.h>
#include <ayni/calls.h>
#include <cstdint>
#include <memory>

namespace ayni::providers::mock {
// A and B have distinct Provider/endpoint IDs and one/two advance ticks of latency.
enum class Variant { a, b };
// Text is a model-class unary echo; action simulates local presentation with progress.
// Typed arguments Core already checked are accepted and ignored.
// Tools produces native function proposals: function:<name> json:<arguments>, or
// call:<binding_id> json:<business arguments> as a fixture shorthand. Unknown
// names and invalid business input reach the normal harness refusal; malformed
// outer lists and capacity failures remain MODEL_OUTPUT. Framed messages select
// only the latest genuine user instruction, preserving numeric source lexemes.
enum class Service { text, action, tools, health };
// Startup bounds; no allocation occurs in submit/cancel/advance.
struct Config {
    Variant variant = Variant::a;
    Service service = Service::text;
    uint32_t request_capacity = 4; // Concurrent borrows, 1..64; terminal post releases one.
    uint32_t text_max_bytes = 512; // Copied input/result bytes per slot, 1..4096.
    // Optional deterministic final text for conversation/summary fixtures, copied
    // at configure (at most 512 bytes). Empty preserves echo/tool behavior. Such
    // fixtures accept up to CALL_RESULT_BYTES_MAX input bytes; tool proposals still take priority.
    ayni_string_v1 reply_text{};
    // Optional explicit fixture definition, immutable and borrowed until close.
    // Action/health consume its root TEXT, or this direct object TEXT field.
    // Core still copies/adopts and validates the complete schema/projection.
    const ayni_business_contract_v1 *business = nullptr;
    uint32_t body_field = 0;
    // Optional for text/tools models: the request body may then also be a body reference,
    // read and verified at submit (ADR-0096). Borrowed until close.
    const ayni_body_store_v1 *bodies = nullptr;
};
// Contract maxima of one configuration: the canonical input and result bytes a Call may carry.
// The runtime sizes the Kernel's Call strides from these, never from its own estimate.
uint32_t call_input_bytes(const Config &config);
uint32_t call_result_bytes(const Config &config);
// Single owner-thread component. Call advance between Kernel steps to deliver
// completions; callbacks only copy/enqueue. No private Kernel state is accessed.
class Provider {
  public:
    Provider();
    // Use close() after successful Kernel destruction. If borrows remain, destruction leaves
    // their context allocated instead of freeing it underneath the Kernel.
    ~Provider();
    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    // Allocate bounded slots and text once; failures leave this object unconfigured.
    ayni_status_t configure(Config config);
    // Copied descriptor; nested offer/resource views live until close. Kernel copies
    // them at registration and borrows the context until it is successfully destroyed.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    // At most one progress/terminal post per occupied slot. BUSY keeps the result
    // for another advance, without re-execution. Other errors are returned intact.
    ayni_status_t advance();
    // Refuses BUSY while a completion is borrowed. Call only after registered
    // Kernels are destroyed (or before registration); success releases all local storage.
    ayni_status_t close();
    // Owner-thread simulated process loss only: end all producers before the old
    // Kernel arena is discarded; no post/receipt is made, execution counts remain.
    void abandon_kernel();
    // Current accepted borrows, including terminal posts waiting for mailbox space.
    uint32_t pending() const;
    // Simulated business executions this configuration; cancellation before execution is excluded.
    uint64_t executions() const;

  private:
    struct Impl;                 // Owns all slots, copied bodies, descriptor views and diagnostics.
    std::unique_ptr<Impl> impl_; // Null exactly when unconfigured/closed.
};
} // namespace ayni::providers::mock
#endif
