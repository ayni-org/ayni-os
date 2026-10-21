// The storage space a RUNNING kernel works within (C-STORE-011, ADR-0027
// decisions 9 and 13): how far the log may grow before a snapshot compacts
// it, and how much free medium a new transition needs.
//
// Two reserves stay free on the medium. The log reserve keeps the log below
// its limit with room for everything the journal can still hold. The
// snapshot reserve is what one compaction writes before it can trim: the
// largest snapshot, its activation batch and two of the largest batches. A
// transition needs both, so the medium never fills up so far that the log
// can neither grow nor be compacted. The free space is the Host's cached,
// nonblocking figure (storage_free_bytes), read on the owner thread.

#ifndef AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H
#define AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H

#include "kernel/kernel_config.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

class StorageBudget {
  public:
    // A fixed margin in the log reserve on top of the journal's bytes.
    static constexpr uint64_t log_reserve_margin_bytes = 1024;
    // storage_log_max_bytes 0 takes the larger of these two (ayni.h).
    static constexpr uint32_t log_max_floor_bytes = 1048576;
    static constexpr uint32_t log_max_buffers = 8;

    // capacities and limits as ayni_create resolved them.
    StorageBudget(const KernelCapacities &capacities, const KernelLimits &limits);

    // The storage_log_max_bytes that 0 selects, and the smallest explicit
    // value ayni_create accepts: the log reserve, two more of the largest
    // batches and an activation batch.
    static uint32_t log_max_default(uint32_t storage_buffer_bytes);
    static uint64_t log_max_min(const KernelCapacities &capacities);

    // Entering RUNNING: the log holds log_bytes, as recovery read them, and
    // nothing asks for a compaction yet.
    void restart(uint64_t log_bytes);
    // A transition's batch of size bytes joined the log.
    void appended(uint64_t size) { log_bytes_ += size; }
    // An activation was stored and the log trimmed to retained bytes.
    void compacted(uint64_t retained);
    // A reload starts over; the compaction this boot asked for is dropped.
    void cancel_compaction() { compaction_due_ = false; }

    // RUNNING with no maintenance active: reads whether the medium still
    // admits a transition, and asks for a compaction once the log reaches
    // three quarters of its limit or the log reserve would cross it. The
    // request stays until compacted() or restart().
    void refresh(const ayni_host_v1 &host);
    bool compaction_due() const { return compaction_due_; }
    // Before a compaction: whether the snapshot reserve is free. When not,
    // transitions stop too, until a refresh() finds the space.
    bool snapshot_fits(const ayni_host_v1 &host);

    // AYNI_OK when the free medium holds both reserves; else
    // AYNI_STORAGE_FAILED (AYNI_REASON_STORAGE_SPACE), also when the Host
    // cannot say.
    ayni_status_t admission(const ayni_host_v1 &host) const;
    // Neither a space shortage nor a pending compaction holds transitions.
    bool admits() const { return !space_blocked_ && !compaction_due_; }

  private:
    // The bytes the log keeps below its limit: the journal buffer, a batch
    // and the margin.
    static uint64_t log_reserve(uint32_t storage_buffer_bytes, uint32_t batch_max);

    uint64_t log_max_bytes_;
    uint64_t log_reserve_;
    uint64_t snapshot_reserve_;
    uint64_t log_bytes_ = 0;
    bool space_blocked_ = false;
    bool compaction_due_ = false;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H
