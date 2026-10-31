// Bounded portable software sum jobs and scalar counter streams; no devices or network.
#ifndef AYNI_PROVIDERS_SOFTWARE_WORK_H
#define AYNI_PROVIDERS_SOFTWARE_WORK_H
#include <ayni/calls.h>
#include <ayni/tool_usage.h>
#include <memory>
namespace ayni::providers::software {
// Caller advances the provider on its owner thread independently of Kernel step.
struct WorkConfig {
    void *clock_context = nullptr;        // Borrowed through close; same clock domain as Kernel.
    uint64_t (*now_us)(void *) = nullptr; // Required bounded monotonic clock.
    uint32_t capacity = 4;                // Positive simultaneous requests, at most capacity_max.
    uint32_t items_per_advance = 128;     // Positive job chunk, at most chunk_max.
    uint64_t event_interval_us = 1000000; // Positive scalar sampling interval.
    uint64_t sample_ttl_us = 2000000;     // Positive source lifetime; not a lease extension.
};
// Sole owner of copied request state and bounded asynchronous progress.
class WorkProvider {
  public:
    static constexpr uint32_t capacity_max = 8;        // Maximum configured request slots.
    static constexpr uint32_t chunk_max = 256;         // Maximum additions in one advance per slot.
    static constexpr int64_t item_count_max = 1000000; // Declared finite job input domain.
    WorkProvider();
    ~WorkProvider(); // A violated drain contract retains borrowed state; close reports BUSY.
    WorkProvider(const WorkProvider &) = delete;
    WorkProvider &operator=(const WorkProvider &) = delete;
    // Allocate bounded slots once; failure preserves the unconfigured instance.
    ayni_status_t configure(WorkConfig config);
    // Copied descriptors borrow this provider through close. Register software.slot at capacity.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    // Exact I64 source software.counter, no media and no hardware assumptions.
    ayni_status_t describe_source(ayni_observation_source_v1 &out) const;
    // Exact local capability/version plus deployment Binding; immutable descriptive data.
    // No authority/registration is created. Description literals are static; output
    // identities borrow the supplied capability/Binding bytes through registration.
    ayni_status_t usage(ayni_string_v1 capability, const ayni_tool_ref_v1 &binding,
                        ayni_tool_usage_v1 &out) const;
    // At most one bounded compute/control/sample/terminal operation per occupied slot.
    ayni_status_t advance();
    uint32_t pending() const; // Outstanding terminal borrows; zero is quiescent.
    // BUSY until every accepted delivery has successfully posted its terminal.
    ayni_status_t close();

  private:
    struct Impl;                 // Owns descriptors, finite leases and copied computation state.
    std::unique_ptr<Impl> impl_; // Null before successful configure/after close.
};
} // namespace ayni::providers::software
#endif
