// Optional low-priority asynchronous rolling A/B audit file sink (ADR-0113).
// Drains ayni_take_audit_records() batches into bounded FileValueRole::background slots.
// Never blocks Core WAL, never restricts Core on flush failures, and owns its worker buffers
// until idle() is true and the Host worker has joined (C-HVAL-001).
#ifndef AYNI_HOSTS_STORAGE_AUDIT_SINK_H
#define AYNI_HOSTS_STORAGE_AUDIT_SINK_H

#include <array>
#include <atomic>
#include <ayni/audit.h>
#include <ayni/hosts/storage/file_value.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ayni::hosts::storage {

// Encoded AYAU v1 binary batch framing (contracts/storage/records.md, ADR-0113).
inline constexpr uint32_t audit_batch_magic = 0x55415941u; // "AYAU"
inline constexpr uint32_t audit_batch_format = 1u;
inline constexpr uint32_t audit_batch_header_bytes = 32u;
inline constexpr uint32_t audit_batch_record_bytes = 100u;

// Decodes one canonical little-endian AYAU v1 audit batch. Returns AYNI_UNSUPPORTED_VERSION
// for unknown formats and AYNI_STORAGE_FAILED for truncated or malformed bytes.
ayni_status_t decode_audit_batch(const uint8_t *data, std::size_t size,
                                 std::vector<ayni_audit_record_v1> &out);

class AuditSink {
  public:
    static constexpr uint64_t default_owner = UINT64_C(0x41594e4941554449); // "AYNIAUDI"
    static constexpr uint32_t slot_count = 2;
    static constexpr uint32_t default_slot_max_bytes = 16384;
    static constexpr uint32_t default_flush_threshold = 16;
    static constexpr uint64_t default_flush_interval_us = 5000000;

    struct Config {
        FileValuePort files{};
        uint64_t owner = default_owner;
        uint32_t slot_max_bytes = default_slot_max_bytes;
        uint32_t flush_threshold = default_flush_threshold;
        uint64_t flush_interval_us = default_flush_interval_us;
    };

    struct Stats {
        uint64_t drained = 0;           // Cumulative records ingested from Core or caller.
        uint64_t flushed_records = 0;   // Cumulative records acknowledged durable in A/B slots.
        uint64_t flushes_completed = 0; // Completed background slot writes.
        uint64_t slot_rotations = 0;    // Active A/B slot rollovers after filling a slot.
        uint64_t write_failures = 0;    // Backpressure rejections or failed background writes.
        uint64_t dropped_records = 0;   // Records dropped when staging hit capacity under failures.
        uint32_t buffered = 0;          // Unflushed records waiting in RAM staging.
        uint32_t active_slot = 0;       // Current target slot index (0 or 1).
        bool in_flight = false;         // True while a FileValuePort borrow is active.
    };

    AuditSink() = default;
    AuditSink(const AuditSink &) = delete;
    AuditSink &operator=(const AuditSink &) = delete;

    ayni_status_t configure(Config config);

    // Owner thread: drains available records from kernel and submits at most one background
    // FileValue write when threshold or interval is reached. Never blocks or fails Core.
    ayni_status_t drain(ayni_kernel_t *kernel, uint64_t now_us);
    // Feeds caller-drained records into the bounded staging queue.
    void ingest(const ayni_audit_record_v1 *records, uint32_t count, uint64_t now_us);
    // Reaps completed background writes and advances pending flush/reset work.
    void poll(uint64_t now_us, bool force_flush = false);
    // Requests immediate background flush of any buffered records (e.g. before shutdown).
    void flush(uint64_t now_us) { poll(now_us, true); }
    // True when no FileValuePort borrow is active (safe to destroy after Host join).
    bool idle() const { return !in_flight_; }
    Stats stats() const;

  private:
    static void on_complete(void *context, const FileValueResult &result) noexcept;
    void reap();
    void submit_next(uint64_t now_us, bool force_flush);
    std::string encode_batch(const std::vector<ayni_audit_record_v1> &records,
                             uint32_t count) const;

    Config config_{};
    bool configured_ = false;
    uint32_t max_records_per_slot_ = 0;
    uint32_t active_slot_ = 0;
    std::array<uint64_t, slot_count> generation_{0, 0};
    std::array<bool, slot_count> needs_reset_{false, false};
    // Prefix [0..slot_flushed_count_) is durable in active_slot_; while a write is in flight,
    // up to one extra slot of newer records may queue behind [0..in_flight_prefix_) so full-slot
    // rotation never drops concurrent arrivals.
    std::vector<ayni_audit_record_v1> slot_records_;
    uint32_t slot_flushed_count_ = 0;  // Prefix of slot_records_ already durable in active_slot_.
    uint64_t oldest_unflushed_us_ = 0; // Timestamp of first unflushed record; 0 when none.

    // Borrowed worker state: mutated only while !in_flight_ or inside on_complete() before release.
    bool in_flight_ = false;
    bool in_flight_remove_ = false;
    bool in_flight_full_ = false; // True iff the active write fills active_slot_ and triggers roll.
    uint32_t in_flight_slot_ = 0;
    uint32_t in_flight_prefix_ = 0; // Prefix of slot_records_ encoded into write_buffer_.
    uint32_t in_flight_delta_ = 0;  // Newly durable records credited on AYNI_OK.
    std::string write_buffer_;
    FileValueResult completion_{};
    std::atomic<bool> completed_{false};

    Stats stats_{};
};

} // namespace ayni::hosts::storage

#endif // AYNI_HOSTS_STORAGE_AUDIT_SINK_H
