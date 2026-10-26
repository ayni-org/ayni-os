// Events posted by Host threads for the owner thread (ADR-0015, ADR-0021).
//
// Wraps the bounded queue with a gate that says whether the kernel takes
// events, so that ayni_post_event, which runs on Host threads, never reads
// the owner's lifecycle field. ayni_start opens the gate and entering
// STOPPED closes it; both, and every queue operation, happen under the
// ingress lock, which the Host's lock makes visible across threads.
//
// Only local inputs are posted so far. Each carries its source in its queue
// entry's body bytes, so every entry keeps room for a catalog id whatever
// event_body_max_bytes says. Only the owner takes events, so the queue
// never shrinks under it.

#ifndef AYNI_CORE_KERNEL_INGRESS_EVENT_INGRESS_H
#define AYNI_CORE_KERNEL_INGRESS_EVENT_INGRESS_H

#include "catalog/catalog_id.h"
#include "kernel/runtime/ingress/event_queue.h"
#include "work/authority/authority_facts.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

class EventIngress {
  public:
    static constexpr std::size_t storage_alignment = EventQueue::storage_alignment;
    static std::size_t storage_size(uint32_t capacity, uint32_t body_max_bytes);

    // storage holds storage_size(capacity, body_max_bytes) bytes aligned to
    // storage_alignment; it and host outlive the ingress. host has both
    // ingress lock functions. 1 <= capacity <= 65535. The gate starts closed.
    EventIngress(void *storage, uint32_t capacity, uint32_t body_max_bytes,
                 const ayni_host_v1 &host);

    EventIngress(const EventIngress &) = delete;
    EventIngress &operator=(const EventIngress &) = delete;

    // Any thread except an ISR. AYNI_OK when queued, AYNI_INVALID_STATE
    // while the gate is closed, AYNI_BUSY when the queue is full, and
    // AYNI_CAPACITY_EXCEEDED when the instance's u64 input order is exhausted.
    // Requires a valid source. Calls no Host function but the lock; the caller
    // wakes the owner after AYNI_OK.
    ayni_status_t post_local_input(const catalog::CatalogId &source);

    // Owner thread.
    void open();
    // Closes the gate for good; returns how many events are still queued,
    // which nobody takes after that.
    uint32_t close();
    uint32_t queued() const;
    // Moves the oldest event out; false when none is queued.
    bool take_local_input(catalog::CatalogId &out_source,
                          work::InputResumeBoundary *out_position = nullptr);
    // Owner supplies the first durable boot epoch once; automatic reloads
    // retain this queue's instance. Producers never read Kernel boot state.
    void establish_incarnation(uint64_t boot_epoch);
    // Samples accepted input order under the producer lock. Exact command
    // retries retain the original sample rather than calling this again.
    work::InputResumeBoundary resume_boundary() const;

  private:
    // Test-only peer sets the otherwise non-enumerable u64 resource boundary.
    // It adds no runtime path, state or test-library dependency to Core.
    friend class EventIngressTestPeer;
    const ayni_host_v1 &host_;
    EventQueue queue_;
    // Lock-owned queue identity/order; zero incarnation before first RUNNING.
    work::InputResumeBoundary accepted_{};
    bool open_ = false;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INGRESS_EVENT_INGRESS_H
