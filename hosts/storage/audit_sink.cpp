// Optional low-priority asynchronous rolling A/B audit file sink (ADR-0113).
#include <ayni/hosts/storage/audit_sink.h>

#include <cstring>

namespace ayni::hosts::storage {
namespace {

constexpr uint32_t byte_bits = 8;
constexpr uint32_t word_bytes = 4;
constexpr uint32_t wide_bytes = 8;
constexpr uint32_t drain_batch_limit = 16;

void append_le(std::string &out, uint64_t value, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        out.push_back(static_cast<char>(value));
        value >>= byte_bits;
    }
}

uint64_t read_le(const uint8_t *data, std::size_t &offset, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t at = 0; at < count; ++at) {
        value |= uint64_t{data[offset++]} << (at * byte_bits);
    }
    return value;
}

} // namespace

ayni_status_t decode_audit_batch(const uint8_t *data, std::size_t size,
                                 std::vector<ayni_audit_record_v1> &out) {
    if (data == nullptr || size < audit_batch_header_bytes) {
        return AYNI_STORAGE_FAILED;
    }
    std::size_t at = 0;
    if (read_le(data, at, word_bytes) != audit_batch_magic) {
        return AYNI_STORAGE_FAILED;
    }
    if (read_le(data, at, word_bytes) != audit_batch_format) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    const auto count = static_cast<uint32_t>(read_le(data, at, word_bytes));
    const auto reserved = static_cast<uint32_t>(read_le(data, at, word_bytes));
    const uint64_t first_seq = read_le(data, at, wide_bytes);
    const uint64_t last_seq = read_le(data, at, wide_bytes);
    const std::size_t expected =
        audit_batch_header_bytes + std::size_t{count} * audit_batch_record_bytes;
    if (reserved != 0U || size != expected ||
        (count == 0U && (first_seq != 0U || last_seq != 0U)) ||
        (count != 0U && first_seq > last_seq)) {
        return AYNI_STORAGE_FAILED;
    }
    std::vector<ayni_audit_record_v1> parsed;
    parsed.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        ayni_audit_record_v1 rec{};
        rec.struct_size = sizeof(rec);
        rec.kind = static_cast<ayni_audit_kind_t>(read_le(data, at, word_bytes));
        rec.seq = read_le(data, at, wide_bytes);
        rec.boot_epoch = read_le(data, at, wide_bytes);
        rec.at_us = read_le(data, at, wide_bytes);
        rec.subject_id = read_le(data, at, wide_bytes);
        rec.auxiliary_id = read_le(data, at, wide_bytes);
        rec.status =
            static_cast<ayni_status_t>(static_cast<uint32_t>(read_le(data, at, word_bytes)));
        rec.reason = static_cast<ayni_reason_t>(read_le(data, at, word_bytes));
        rec.detail_code = static_cast<uint32_t>(read_le(data, at, word_bytes));
        rec.flags = static_cast<uint32_t>(read_le(data, at, word_bytes));
        rec.label_size = static_cast<uint32_t>(read_le(data, at, word_bytes));
        rec.reserved = static_cast<uint32_t>(read_le(data, at, word_bytes));
        if (rec.label_size > AYNI_CATALOG_ID_MAX_BYTES || rec.reserved != 0U) {
            return AYNI_STORAGE_FAILED;
        }
        std::memcpy(rec.label, data + at, AYNI_CATALOG_ID_MAX_BYTES);
        at += AYNI_CATALOG_ID_MAX_BYTES;
        parsed.push_back(rec);
    }
    if (count != 0U && (parsed.front().seq != first_seq || parsed.back().seq != last_seq)) {
        return AYNI_STORAGE_FAILED;
    }
    out = std::move(parsed);
    return AYNI_OK;
}

ayni_status_t AuditSink::configure(Config config) {
    if (configured_ || in_flight_) {
        return AYNI_INVALID_STATE;
    }
    if (!config.files.submit || !config.owner || !config.flush_threshold ||
        config.slot_max_bytes < audit_batch_header_bytes + audit_batch_record_bytes ||
        config.slot_max_bytes > file_value_bytes_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    config_ = config;
    max_records_per_slot_ =
        (config_.slot_max_bytes - audit_batch_header_bytes) / audit_batch_record_bytes;
    slot_records_.reserve(max_records_per_slot_);
    configured_ = true;
    return AYNI_OK;
}

void AuditSink::on_complete(void *context, const FileValueResult &result) noexcept {
    auto &self = *static_cast<AuditSink *>(context);
    self.completion_ = result;
    self.completed_.store(true, std::memory_order_release);
}

std::string AuditSink::encode_batch(const std::vector<ayni_audit_record_v1> &records) const {
    std::string out;
    out.reserve(audit_batch_header_bytes + records.size() * audit_batch_record_bytes);
    append_le(out, audit_batch_magic, word_bytes);
    append_le(out, audit_batch_format, word_bytes);
    append_le(out, records.size(), word_bytes);
    append_le(out, 0, word_bytes);
    append_le(out, records.empty() ? 0U : records.front().seq, wide_bytes);
    append_le(out, records.empty() ? 0U : records.back().seq, wide_bytes);
    for (const auto &rec : records) {
        append_le(out, rec.kind, word_bytes);
        append_le(out, rec.seq, wide_bytes);
        append_le(out, rec.boot_epoch, wide_bytes);
        append_le(out, rec.at_us, wide_bytes);
        append_le(out, rec.subject_id, wide_bytes);
        append_le(out, rec.auxiliary_id, wide_bytes);
        append_le(out, static_cast<uint32_t>(rec.status), word_bytes);
        append_le(out, rec.reason, word_bytes);
        append_le(out, rec.detail_code, word_bytes);
        append_le(out, rec.flags, word_bytes);
        append_le(out, rec.label_size, word_bytes);
        append_le(out, 0, word_bytes);
        out.append(rec.label, AYNI_CATALOG_ID_MAX_BYTES);
    }
    return out;
}

void AuditSink::reap() {
    if (!in_flight_ || !completed_.exchange(false, std::memory_order_acquire)) {
        return;
    }
    in_flight_ = false;
    if (in_flight_remove_) {
        in_flight_remove_ = false;
        if (completion_.status == AYNI_OK) {
            generation_[in_flight_slot_] = 0;
            needs_reset_[in_flight_slot_] = false;
        } else {
            ++stats_.write_failures;
        }
        return;
    }
    if (completion_.status == AYNI_OK) {
        generation_[in_flight_slot_] = completion_.generation;
        if (in_flight_slot_ == active_slot_) {
            slot_flushed_count_ = in_flight_record_count_;
        }
        stats_.flushed_records += in_flight_delta_;
        ++stats_.flushes_completed;
        if (slot_flushed_count_ >= slot_records_.size()) {
            oldest_unflushed_us_ = 0;
        }
        // Roll over to the alternate A/B slot once the active slot reaches full capacity.
        if (in_flight_slot_ == active_slot_ && slot_flushed_count_ >= max_records_per_slot_) {
            active_slot_ = (active_slot_ + 1U) % slot_count;
            slot_records_.clear();
            slot_flushed_count_ = 0;
            oldest_unflushed_us_ = 0;
            ++stats_.slot_rotations;
        }
        return;
    }
    if (completion_.status == AYNI_STALE) {
        // Previous boot owned this rolling slot with an unknown generation; remove before rewrite.
        needs_reset_[in_flight_slot_] = true;
        return;
    }
    ++stats_.write_failures;
}

void AuditSink::ingest(const ayni_audit_record_v1 *records, uint32_t count, uint64_t now_us) {
    if (!configured_ || records == nullptr || count == 0U) {
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (slot_records_.size() >= max_records_per_slot_) {
            if (!in_flight_ && slot_flushed_count_ >= max_records_per_slot_) {
                active_slot_ = (active_slot_ + 1U) % slot_count;
                slot_records_.clear();
                slot_flushed_count_ = 0;
                oldest_unflushed_us_ = 0;
                ++stats_.slot_rotations;
            } else {
                // Staging full while storage is busy/failing: drop oldest record to preserve bound.
                slot_records_.erase(slot_records_.begin());
                if (slot_flushed_count_ > 0U) {
                    --slot_flushed_count_;
                }
                ++stats_.dropped_records;
            }
        }
        if (slot_records_.size() == slot_flushed_count_) {
            oldest_unflushed_us_ = now_us;
        }
        slot_records_.push_back(records[index]);
        ++stats_.drained;
    }
}

void AuditSink::submit_next(uint64_t now_us, bool force_flush) {
    if (!configured_ || in_flight_) {
        return;
    }
    if (needs_reset_[active_slot_]) {
        FileValueRequest request{};
        request.kind = FileValueRequest::Kind::remove;
        request.priority = FileValueRequest::Priority::background;
        request.role = FileValueRole::other;
        request.key = {config_.owner, active_slot_ + 1U};
        request.context = this;
        request.complete = on_complete;
        if (config_.files.submit(config_.files.context, &request) == AYNI_SUBMIT_ACCEPTED) {
            in_flight_ = true;
            in_flight_remove_ = true;
            in_flight_slot_ = active_slot_;
        } else {
            ++stats_.write_failures;
        }
        return;
    }
    const uint32_t unflushed = static_cast<uint32_t>(slot_records_.size() - slot_flushed_count_);
    if (unflushed == 0U) {
        return;
    }
    const bool interval_due = oldest_unflushed_us_ != 0U && now_us >= oldest_unflushed_us_ &&
                              now_us - oldest_unflushed_us_ >= config_.flush_interval_us;
    const bool slot_full = slot_records_.size() >= max_records_per_slot_;
    if (!force_flush && !slot_full && unflushed < config_.flush_threshold && !interval_due) {
        return;
    }
    write_buffer_ = encode_batch(slot_records_);
    FileValueRequest request{};
    request.kind = FileValueRequest::Kind::write;
    request.priority = FileValueRequest::Priority::background;
    request.role = FileValueRole::other;
    request.key = {config_.owner, active_slot_ + 1U};
    request.previous_generation = generation_[active_slot_];
    request.generation = generation_[active_slot_] + 1U;
    request.data = reinterpret_cast<const uint8_t *>(write_buffer_.data());
    request.size = static_cast<uint32_t>(write_buffer_.size());
    request.context = this;
    request.complete = on_complete;
    if (config_.files.submit(config_.files.context, &request) == AYNI_SUBMIT_ACCEPTED) {
        in_flight_ = true;
        in_flight_remove_ = false;
        in_flight_slot_ = active_slot_;
        in_flight_record_count_ = static_cast<uint32_t>(slot_records_.size());
        in_flight_delta_ = unflushed;
    } else {
        ++stats_.write_failures;
    }
}

void AuditSink::poll(uint64_t now_us, bool force_flush) {
    reap();
    submit_next(now_us, force_flush);
}

ayni_status_t AuditSink::drain(ayni_kernel_t *kernel, uint64_t now_us) {
    if (!configured_ || kernel == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    reap();
    std::array<ayni_audit_record_v1, drain_batch_limit> batch{};
    uint32_t taken = 0;
    const auto status = ayni_take_audit_records(kernel, batch.data(), drain_batch_limit, &taken);
    if (status != AYNI_OK) {
        return status;
    }
    if (taken != 0U) {
        ingest(batch.data(), taken, now_us);
    }
    submit_next(now_us, false);
    return AYNI_OK;
}

AuditSink::Stats AuditSink::stats() const {
    Stats out = stats_;
    out.buffered = static_cast<uint32_t>(slot_records_.size() - slot_flushed_count_);
    out.active_slot = active_slot_;
    out.in_flight = in_flight_;
    return out;
}

} // namespace ayni::hosts::storage
