// Fixed-capacity RAM overwrite-oldest audit ring (ADR-0113).
// Owned exclusively by ayni_kernel on the owner thread; never allocates, blocks,
// or writes to the synchronous WAL. Capacity 0 consumes zero arena bytes.
#ifndef AYNI_CORE_KERNEL_SERVICE_AUDIT_AUDIT_RING_H
#define AYNI_CORE_KERNEL_SERVICE_AUDIT_AUDIT_RING_H

#include <ayni/audit.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

// Structured inputs for AuditRing::emit(); static labels only, never user text.
struct AuditEntry {
    ayni_audit_kind_t kind = 0; // Required AYNI_AUDIT_* category.
    uint64_t boot_epoch = 0;    // Current Kernel boot epoch; 0 before recovery settles.
    uint64_t at_us = 0;         // Host monotonic timestamp in microseconds.
    uint64_t subject_id = 0;    // Primary object ID (call_id, task_id, or 0).
    uint64_t auxiliary_id = 0;  // Secondary ID (task_id, preemptor, permit_id, or 0).
    ayni_status_t status = AYNI_OK;
    ayni_reason_t reason = AYNI_REASON_NONE;
    uint32_t detail_code = 0; // Kind-specific enum (outcome, reflex action, lifecycle).
    uint32_t flags = 0;       // AYNI_AUDIT_FLAG_* bitmask.
    ayni_string_v1 label{};   // Optional static catalog/rule/resource identifier.
};

// Bounded circular buffer of ayni_audit_record_v1 in the caller-supplied Kernel arena.
// Invariants: count_ <= capacity_; head_ < capacity_ when capacity_ > 0; next_seq_ >= 1.
class AuditRing {
  public:
    static constexpr std::size_t storage_alignment = alignof(ayni_audit_record_v1);

    // Total arena bytes required for capacity slots; returns 0 when disabled.
    static std::size_t storage_size(uint32_t capacity) {
        return static_cast<std::size_t>(capacity) * sizeof(ayni_audit_record_v1);
    }

    AuditRing(void *storage, uint32_t capacity)
        : records_(capacity == 0U ? nullptr : static_cast<ayni_audit_record_v1 *>(storage)),
          capacity_(capacity) {}
    AuditRing(const AuditRing &) = delete;
    AuditRing &operator=(const AuditRing &) = delete;

    // Appends one record in O(1). When full, overwrites the oldest unread slot and increments
    // dropped_overflow_. No-op when disabled (capacity_ == 0).
    void emit(const AuditEntry &entry);

    // Copies up to max_count oldest unread records into out in FIFO seq order and advances head_.
    uint32_t take(ayni_audit_record_v1 *out, uint32_t max_count);

    // Populates diagnostics into an already size-validated ayni_audit_info_v1.
    void describe(ayni_audit_info_v1 &out) const;

    uint32_t capacity() const { return capacity_; }

  private:
    ayni_audit_record_v1 *records_ = nullptr; // Arena-backed ring array; null iff capacity_ == 0.
    uint32_t capacity_ = 0;                   // Fixed slot count for Kernel lifetime; 0 disables.
    uint32_t head_ = 0;                       // Index of oldest unread slot (0 when empty).
    uint32_t count_ = 0;                      // Current unread slot count, 0..capacity_.
    uint64_t next_seq_ = 1;                   // Strictly monotonic 1-based sequence counter.
    uint64_t dropped_overflow_ = 0;           // Cumulative unread records lost to wrap-around.
    uint64_t drained_total_ = 0;              // Cumulative records copied out by take().
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_SERVICE_AUDIT_AUDIT_RING_H
