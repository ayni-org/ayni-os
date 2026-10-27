// Deterministic finite camera subscription with opaque Mock bytes; no real capture.
#ifndef AYNI_PROVIDERS_MOCK_SUBSCRIPTION_H
#define AYNI_PROVIDERS_MOCK_SUBSCRIPTION_H
#include <ayni/providers/mock.h>
#include <ayni/providers/mock/media.h>
namespace ayni::providers::mock {
// Fixed startup bounds; clock belongs to the same monotonic domain as Kernel requests.
struct SubscriptionConfig {
    Variant variant = Variant::a;
    uint32_t request_capacity = 4; // 1..64 concurrent generation borrows.
    uint32_t media_objects = 8;    // 1..64 independent bounded three-byte frames.
    uint32_t lease_ms = AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT; // 0 selects the public default.
    uint64_t frame_interval_us = 1000000; // Positive finite minimum sample interval.
    uint32_t change_every_frames =
        0; // 0 keeps scalar 1; otherwise alternate 0/1 per this many frames.
    uint64_t sample_ttl_us =
        2000000;                   // Positive finite source validity; bytes also bounded by lease.
    void *clock_context = nullptr; // Borrowed until close.
    uint64_t (*now_us)(void *) = nullptr; // Required nonblocking owner clock.
};
// Single owner-thread component: advance independently of Kernel step to enforce
// lease expiry and return its borrow. No allocation in submit/renew/cancel/advance.
class SubscriptionProvider {
  public:
    SubscriptionProvider();
    // Close after successful Kernel destruction. A prematurely destroyed provider
    // retains a borrowed context rather than freeing memory underneath Core.
    ~SubscriptionProvider();
    SubscriptionProvider(const SubscriptionProvider &) = delete;
    SubscriptionProvider &operator=(const SubscriptionProvider &) = delete;
    // Allocate fixed request slots and MediaStore once; preserve unconfigured state on failure.
    ayni_status_t configure(SubscriptionConfig config);
    // Copied descriptors, nested views last through close; caller registers Resource camera.slot.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    ayni_status_t describe_source(ayni_observation_source_v1 &out) const;
    // At most one control/sample/terminal operation per occupied request per call.
    // FIFO BUSY retains the frame for a later advance; known losses report reliable gap.
    ayni_status_t advance();
    // BUSY until every original delivery borrow was returned by terminal post.
    ayni_status_t close();
    uint32_t pending() const; // Accepted unreturned generation borrows.
    // Read-only store capability for explicitly authorized fixture reads; null unconfigured.
    const MediaStore *media_store() const;

  private:
    struct Impl; // Owns descriptors, bounded slots, current generation leases and Mock bytes.
    std::unique_ptr<Impl> impl_; // Null until configure succeeds or after close.
};
} // namespace ayni::providers::mock
#endif
