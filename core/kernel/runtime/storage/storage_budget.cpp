#include "kernel/runtime/storage/storage_budget.h"

#include "durability/batch/batch_codec.h"
#include "durability/snapshot/snapshot_codec.h"
#include "kernel/runtime/storage/write_schedule.h"
#include "kernel/service/persistence/work_journal.h"
#include "kernel/service/persistence/work_snapshot.h"

namespace ayni::kernel {

StorageBudget::StorageBudget(const KernelCapacities &capacities, const KernelLimits &limits)
    : log_max_bytes_(limits.storage_log_max_bytes),
      log_reserve_(log_reserve(capacities.storage_buffer_bytes, transition_batch_max(capacities))),
      snapshot_reserve_(snapshot_payload_max(capacities) + durability::snapshot_header_size +
                        durability::snapshot_marker_size + durability::snapshot_commit_batch_size +
                        2U * uint64_t{transition_batch_max(capacities)}) {}

uint64_t StorageBudget::log_reserve(uint32_t storage_buffer_bytes, uint32_t batch_max) {
    return uint64_t{storage_buffer_bytes} + batch_max + log_reserve_margin_bytes;
}

uint32_t StorageBudget::log_max_default(uint32_t storage_buffer_bytes) {
    // Both explicit and automatically derived schema-4 maxima fit uint32_t
    // after this multiplication. Automatic sizing may exceed the explicit input
    // limit when complete proof budgets are large (records.md / ADR-0084).
    return static_cast<uint32_t>(
        storage_max(uint64_t{log_max_buffers} * storage_buffer_bytes, log_max_floor_bytes));
}

uint64_t StorageBudget::log_max_min(const KernelCapacities &capacities) {
    const uint32_t batch_max = transition_batch_max(capacities);
    return log_reserve(capacities.storage_buffer_bytes, batch_max) + 2U * uint64_t{batch_max} +
           durability::snapshot_commit_batch_size;
}

void StorageBudget::restart(uint64_t log_bytes) {
    log_bytes_ = log_bytes;
    compaction_due_ = false;
}

void StorageBudget::compacted(uint64_t retained) {
    log_bytes_ = retained;
    compaction_due_ = false;
}

void StorageBudget::refresh(const ayni_host_v1 &host) {
    space_blocked_ = admission(host) != AYNI_OK;
    if (log_bytes_ >= compaction_threshold(log_max_bytes_) ||
        log_bytes_ + log_reserve_ >= log_max_bytes_) {
        compaction_due_ = true;
    }
}

bool StorageBudget::refresh_for_snapshot(const ayni_host_v1 &host) {
    uint64_t available = 0;
    if (host.storage_free_bytes(host.ctx, &available) != AYNI_OK || available < snapshot_reserve_) {
        space_blocked_ = true;
        return false;
    }
    return true;
}

ayni_status_t StorageBudget::admission(const ayni_host_v1 &host) const {
    uint64_t available = 0;
    if (host.storage_free_bytes == nullptr ||
        host.storage_free_bytes(host.ctx, &available) != AYNI_OK ||
        available < storage_max(snapshot_reserve_, log_reserve_)) {
        return AYNI_STORAGE_FAILED;
    }
    return AYNI_OK;
}

} // namespace ayni::kernel
