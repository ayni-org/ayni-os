// The storage space a RUNNING kernel works within (C-STORE-011, ADR-0027
// decision 9): how far the log may grow before a snapshot compacts it, and
// how much free medium a new transition needs.
//
// There are two reserves. The log reserve is the journal buffer, one more
// of the largest batches and a margin: a compaction is asked for once the
// log comes that close to its limit. The snapshot reserve is the largest
// slot (header, payload and marker), its activation batch and two more of
// the largest batches: a compaction starts only with that much free. A
// transition is admitted while the free medium holds the larger of the two.
// The free space is the Host's cached, nonblocking figure
// (storage_free_bytes), read on the owner thread.

#ifndef AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H
#define AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H

#include "kernel/config/kernel_config.h"

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

    // With no maintenance active: reads whether the medium still
    // admits a transition, and asks for a compaction once the log reaches
    // three quarters of its limit or the log reserve would cross it. The
    // request stays until compacted(), cancel_compaction() or restart().
    void refresh(const ayni_host_v1 &host);
    bool compaction_due() const { return compaction_due_; }
    // Before a compaction: whether the snapshot reserve is free. When not,
    // it also holds transitions, as a shortage refresh() finds does, until
    // a refresh() finds the space.
    bool refresh_for_snapshot(const ayni_host_v1 &host);

    // AYNI_OK when the free medium holds the larger reserve; else
    // AYNI_STORAGE_FAILED (AYNI_REASON_STORAGE_SPACE), also when the Host
    // cannot say.
    ayni_status_t admission(const ayni_host_v1 &host) const;
    // Neither a space shortage nor a pending compaction holds transitions.
    bool admits() const { return !space_blocked_ && !compaction_due_; }

  private:
    // The bytes the log keeps below its limit: the journal buffer, a batch
    // and the margin.
    static uint64_t log_reserve(uint32_t storage_buffer_bytes, uint32_t batch_max);
    // Where the log asks for a compaction whatever the reserve: three
    // quarters of the limit (ADR-0027 decision 9).
    static uint64_t compaction_threshold(uint64_t log_max_bytes) { return log_max_bytes * 3U / 4U; }

    uint64_t log_max_bytes_;
    uint64_t log_reserve_;
    uint64_t snapshot_reserve_;
    uint64_t log_bytes_ = 0;
    bool space_blocked_ = false;
    bool compaction_due_ = false;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_STORAGE_STORAGE_BUDGET_H
