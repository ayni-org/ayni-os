// Fixed-capacity RAM overwrite-oldest audit ring implementation (ADR-0113).
#include "kernel/service/audit/audit_ring.h"

#include <cstring>

namespace ayni::kernel {

// Appends one record without blocking or failing. Full rings overwrite the oldest unread entry
// so Core hot paths never stall behind slow Host drains; gaps in seq prove overflow.
void AuditRing::emit(const AuditEntry &entry) {
    if (capacity_ == 0U || records_ == nullptr) {
        return;
    }
    uint32_t slot = 0;
    if (count_ == capacity_) {
        slot = head_;
        head_ = (head_ + 1U) % capacity_;
        ++dropped_overflow_;
    } else {
        slot = (head_ + count_) % capacity_;
        ++count_;
    }
    ayni_audit_record_v1 record{};
    record.struct_size = sizeof(record);
    record.kind = entry.kind;
    record.seq = next_seq_++;
    record.boot_epoch = entry.boot_epoch;
    record.at_us = entry.at_us;
    record.subject_id = entry.subject_id;
    record.auxiliary_id = entry.auxiliary_id;
    record.status = entry.status;
    record.reason = entry.reason;
    record.detail_code = entry.detail_code;
    record.flags = entry.flags;
    if (entry.label.data != nullptr && entry.label.size != 0U) {
        const uint32_t copy = entry.label.size <= AYNI_CATALOG_ID_MAX_BYTES
                                  ? entry.label.size
                                  : AYNI_CATALOG_ID_MAX_BYTES;
        std::memcpy(record.label, entry.label.data, copy);
        record.label_size = copy;
    }
    records_[slot] = record;
}

uint32_t AuditRing::take(ayni_audit_record_v1 *out, uint32_t max_count) {
    if (capacity_ == 0U || records_ == nullptr || count_ == 0U || max_count == 0U ||
        out == nullptr) {
        return 0U;
    }
    const uint32_t take_count = count_ < max_count ? count_ : max_count;
    for (uint32_t index = 0; index < take_count; ++index) {
        out[index] = records_[head_];
        head_ = (head_ + 1U) % capacity_;
    }
    count_ -= take_count;
    drained_total_ += take_count;
    return take_count;
}

void AuditRing::describe(ayni_audit_info_v1 &out) const {
    out = {};
    out.struct_size = sizeof(out);
    out.capacity = capacity_;
    out.pending = count_;
    out.next_seq = next_seq_;
    out.dropped_overflow = dropped_overflow_;
    out.drained_total = drained_total_;
}

} // namespace ayni::kernel
