// ADR-0113: portable Host AuditSink verification (AYAU v1 codec, A/B dual-slot rolling files,
// background FileValue priority, previous-boot STALE recovery, and complete failure isolation).

#include "work_node.h"

#include <ayni/audit.h>
#include <ayni/hosts/storage/audit_sink.h>
#include <ayni/hosts/storage/file_value.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace ayni::hosts::storage;
using namespace ayni::test;

// Deterministic in-memory Files adapter for portable FileValue / AuditSink tests.
class MemoryFiles final : public Files {
  public:
    int begin(const char *) override { return 0; }
    void end() override { handles_.clear(); }
    int open(const char *name, Mode mode, int &handle) override {
        const std::string key(name);
        auto it = files_.find(key);
        if (mode == Mode::read_existing || mode == Mode::read_existing_no_follow) {
            if (it == files_.end()) {
                return ENOENT;
            }
        } else if (mode == Mode::replace) {
            files_[key].clear();
        } else {
            files_[key];
        }
        handle = next_handle_++;
        handles_[handle] = key;
        return 0;
    }
    void close(int handle) override { handles_.erase(handle); }
    int lock(int) override { return 0; }
    int read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
             std::size_t &count) const override {
        count = 0;
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        if (f == files_.end() || offset >= f->second.size()) {
            return 0;
        }
        count = std::min<std::size_t>(bytes, f->second.size() - static_cast<std::size_t>(offset));
        std::memcpy(data, f->second.data() + offset, count);
        return 0;
    }
    int write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        auto &buf = files_[h->second];
        const auto end = static_cast<std::size_t>(offset) + bytes;
        if (buf.size() < end) {
            buf.resize(end);
        }
        if (bytes != 0U) {
            std::memcpy(buf.data() + offset, data, bytes);
        }
        return 0;
    }
    int size(int handle, uint64_t &bytes) const override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        bytes = f == files_.end() ? 0 : f->second.size();
        return 0;
    }
    int truncate(int handle, uint64_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        files_[h->second].resize(static_cast<std::size_t>(bytes));
        return 0;
    }
    int sync_file(int) override { return 0; }
    int sync_namespace() override { return 0; }
    int rename(const char *from, const char *to) override {
        auto it = files_.find(from);
        if (it == files_.end()) {
            return ENOENT;
        }
        files_[to] = std::move(it->second);
        files_.erase(it);
        return 0;
    }
    int remove(const char *name) override {
        const auto erased = files_.erase(name);
        return erased ? 0 : ENOENT;
    }
    int free_bytes(uint64_t &bytes) const override {
        bytes = 4U * 1024U * 1024U;
        return 0;
    }

  private:
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<int, std::string> handles_;
    int next_handle_ = 1;
};

struct QueuedPort {
    MemoryFiles files;
    std::deque<FileValueRequest> queue;
    bool reject_submits = false;
    bool fail_writes = false;
    uint32_t background_submits = 0;

    FileValuePort port() { return {this, submit}; }

    static ayni_submit_result_t submit(void *ctx, const FileValueRequest *req) {
        auto &self = *static_cast<QueuedPort *>(ctx);
        if (self.reject_submits) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        if (req->priority == FileValueRequest::Priority::background) {
            ++self.background_submits;
        }
        self.queue.push_back(*req);
        return AYNI_SUBMIT_ACCEPTED;
    }

    void drain_one() {
        if (queue.empty()) {
            return;
        }
        const auto req = queue.front();
        queue.pop_front();
        FileValueResult res{};
        if (fail_writes && req.kind == FileValueRequest::Kind::write) {
            res.status = AYNI_STORAGE_FAILED;
        } else {
            res = serve_file_value(files, req);
        }
        req.complete(req.context, res);
    }

    void drain_all() {
        while (!queue.empty()) {
            drain_one();
        }
    }

    std::vector<ayni_audit_record_v1> read_slot(uint64_t owner, uint64_t object) {
        std::vector<uint8_t> raw(file_value_bytes_max);
        FileValueRequest req{};
        req.kind = FileValueRequest::Kind::read;
        req.key = {owner, object};
        req.buffer = raw.data();
        req.size = static_cast<uint32_t>(raw.size());
        req.complete = [](void *, const FileValueResult &) noexcept {};
        const auto res = serve_file_value(files, req);
        EXPECT_EQ(res.status, AYNI_OK);
        std::vector<ayni_audit_record_v1> decoded;
        EXPECT_EQ(decode_audit_batch(raw.data(), res.size, decoded), AYNI_OK);
        return decoded;
    }
};

ayni_audit_record_v1 make_record(uint64_t seq, const char *label) {
    ayni_audit_record_v1 rec{};
    rec.struct_size = sizeof(rec);
    rec.kind = AYNI_AUDIT_CALL_TERMINAL;
    rec.seq = seq;
    rec.boot_epoch = 3;
    rec.at_us = seq * 1000;
    rec.subject_id = seq + 100;
    rec.auxiliary_id = 7;
    rec.status = AYNI_OK;
    rec.reason = AYNI_REASON_NONE;
    rec.detail_code = AYNI_CALL_COMPLETED;
    rec.flags = AYNI_AUDIT_FLAG_EPHEMERAL;
    rec.label_size = static_cast<uint32_t>(std::strlen(label));
    std::memcpy(rec.label, label, rec.label_size);
    return rec;
}

// ADR-0113: AuditSink drains live Core records at background priority, rotates A/B slots cleanly,
// and round-trips exact AYAU v1 binary records.
TEST(AuditSink, drains_kernel_rotates_ab_slots_and_round_trips_ayau_v1) {
    QueuedPort port;
    AuditSink sink;
    AuditSink::Config bad{};
    EXPECT_EQ(sink.configure(bad), AYNI_INVALID_ARGUMENT);

    // Slot fits exactly 3 records: 32 + 3 * 100 = 332 bytes.
    AuditSink::Config cfg{};
    cfg.files = port.port();
    cfg.slot_max_bytes = audit_batch_header_bytes + 3U * audit_batch_record_bytes;
    cfg.flush_threshold = 2;
    cfg.flush_interval_us = 1000000;
    ASSERT_EQ(sink.configure(cfg), AYNI_OK);
    EXPECT_EQ(sink.configure(cfg), AYNI_INVALID_STATE);

    // Feed 5 records across two slots (slot 1 gets seq 1..3, rotates to slot 2 for seq 4..5).
    for (uint64_t seq = 1; seq <= 5; ++seq) {
        const auto rec = make_record(seq, "motor.run");
        sink.ingest(&rec, 1, seq * 10000);
        sink.poll(seq * 10000);
        port.drain_all();
        sink.poll(seq * 10000);
    }
    sink.flush(100000);
    port.drain_all();
    sink.poll(100000);
    EXPECT_TRUE(sink.idle());

    const auto st = sink.stats();
    EXPECT_EQ(st.drained, 5U);
    EXPECT_EQ(st.flushed_records, 5U);
    EXPECT_EQ(st.slot_rotations, 1U);
    EXPECT_EQ(st.write_failures, 0U);
    EXPECT_EQ(st.dropped_records, 0U);
    EXPECT_GT(port.background_submits, 0U);

    const auto slot_a = port.read_slot(AuditSink::default_owner, 1);
    const auto slot_b = port.read_slot(AuditSink::default_owner, 2);
    ASSERT_EQ(slot_a.size(), 3U);
    ASSERT_EQ(slot_b.size(), 2U);
    EXPECT_EQ(slot_a[0].seq, 1U);
    EXPECT_EQ(slot_a[2].seq, 3U);
    EXPECT_EQ(slot_b[0].seq, 4U);
    EXPECT_EQ(slot_b[1].seq, 5U);
    EXPECT_EQ(std::string_view(slot_b[1].label, slot_b[1].label_size), "motor.run");

    // Live Kernel drain integration.
    WorkNode work;
    work.node().config().audit_capacity = 16;
    work.run();
    ASSERT_EQ(sink.drain(work.kernel(), 200000), AYNI_OK);
    sink.flush(200000);
    port.drain_all();
    sink.poll(200000);
    EXPECT_TRUE(sink.idle());
    EXPECT_GT(sink.stats().drained, 5U);
}

// ADR-0113: recovers from stale previous-boot slot generation, isolates queue/storage failures,
// and rejects unknown AYAU versions cleanly.
TEST(AuditSink, recovers_from_stale_slot_isolates_failures_and_validates_format) {
    QueuedPort port;
    // Pre-populate slot 1 with generation 5 (simulating a previous boot's file).
    const std::string stale_data = "previous-boot";
    FileValueRequest seed{};
    seed.kind = FileValueRequest::Kind::write;
    seed.key = {AuditSink::default_owner, 1};
    seed.previous_generation = 0;
    seed.generation = 5;
    seed.data = reinterpret_cast<const uint8_t *>(stale_data.data());
    seed.size = static_cast<uint32_t>(stale_data.size());
    seed.complete = [](void *, const FileValueResult &) noexcept {};
    ASSERT_EQ(serve_file_value(port.files, seed).status, AYNI_OK);

    AuditSink sink;
    AuditSink::Config cfg{};
    cfg.files = port.port();
    cfg.slot_max_bytes = audit_batch_header_bytes + 2U * audit_batch_record_bytes;
    cfg.flush_threshold = 1;
    ASSERT_EQ(sink.configure(cfg), AYNI_OK);

    const auto r1 = make_record(10, "reflex.stop");
    sink.ingest(&r1, 1, 1000);
    // 1st attempt hits STALE -> schedules background remove -> 2nd write succeeds at generation 1.
    for (int step = 0; step < 5; ++step) {
        sink.poll(1000 + uint64_t(step) * 100);
        port.drain_all();
    }
    EXPECT_TRUE(sink.idle());
    EXPECT_EQ(sink.stats().flushed_records, 1U);
    const auto recovered = port.read_slot(AuditSink::default_owner, 1);
    ASSERT_EQ(recovered.size(), 1U);
    EXPECT_EQ(recovered[0].seq, 10U);

    // Queue rejections and storage write failures never hang or break bounded staging.
    port.reject_submits = true;
    for (uint64_t seq = 11; seq <= 15; ++seq) {
        const auto rec = make_record(seq, "busy.cap");
        sink.ingest(&rec, 1, seq * 1000);
        sink.poll(seq * 1000);
    }
    EXPECT_TRUE(sink.idle());
    EXPECT_GT(sink.stats().write_failures, 0U);
    EXPECT_GT(sink.stats().dropped_records, 0U);

    // AYAU unknown format returns AYNI_UNSUPPORTED_VERSION; malformed returns AYNI_STORAGE_FAILED.
    std::vector<uint8_t> bad_ver(audit_batch_header_bytes, 0);
    bad_ver[0] = 'A';
    bad_ver[1] = 'Y';
    bad_ver[2] = 'A';
    bad_ver[3] = 'U';
    bad_ver[4] = 99; // Unknown format version.
    std::vector<ayni_audit_record_v1> parsed;
    EXPECT_EQ(decode_audit_batch(bad_ver.data(), bad_ver.size(), parsed), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(decode_audit_batch(bad_ver.data(), 8, parsed), AYNI_STORAGE_FAILED);
}

// ADR-0113: records arriving while a full-slot write is in flight survive slot rotation without
// silent loss, and decode_audit_batch rejects 32-bit multiplication-overflow counts cleanly.
TEST(AuditSink, preserves_records_ingested_during_in_flight_full_slot_write_and_rejects_overflow) {
    QueuedPort port;
    AuditSink sink;
    AuditSink::Config cfg{};
    cfg.files = port.port();
    cfg.slot_max_bytes = audit_batch_header_bytes + 3U * audit_batch_record_bytes;
    cfg.flush_threshold = 3;
    cfg.flush_interval_us = 1000000;
    ASSERT_EQ(sink.configure(cfg), AYNI_OK);

    // Fill slot 1 (seq 1..3) and submit write WITHOUT completing the worker yet.
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        const auto rec = make_record(seq, "slot1.full");
        sink.ingest(&rec, 1, seq * 1000);
    }
    sink.poll(3000);
    ASSERT_FALSE(sink.idle());

    // While slot 1 write is still in flight, ingest seq 4..5 -> must queue for slot 2.
    for (uint64_t seq = 4; seq <= 5; ++seq) {
        const auto rec = make_record(seq, "slot2.next");
        sink.ingest(&rec, 1, seq * 1000);
    }

    // Complete slot 1 write, rotate to slot 2, and force-flush seq 4..5 into slot 2.
    port.drain_all();
    sink.flush(10000);
    port.drain_all();
    sink.poll(10000);
    EXPECT_TRUE(sink.idle());

    const auto st = sink.stats();
    EXPECT_EQ(st.drained, 5U);
    EXPECT_EQ(st.flushed_records, 5U);
    EXPECT_EQ(st.dropped_records, 0U);
    EXPECT_EQ(st.slot_rotations, 1U);

    const auto slot_a = port.read_slot(AuditSink::default_owner, 1);
    const auto slot_b = port.read_slot(AuditSink::default_owner, 2);
    ASSERT_EQ(slot_a.size(), 3U);
    ASSERT_EQ(slot_b.size(), 2U);
    EXPECT_EQ(slot_a[0].seq, 1U);
    EXPECT_EQ(slot_a[2].seq, 3U);
    EXPECT_EQ(slot_b[0].seq, 4U);
    EXPECT_EQ(slot_b[1].seq, 5U);

    // 32-bit overflow trigger: 0x028F5C29u * 100 == 4 (mod 2^32) -> 32 + 4 = 36 B packet.
    std::vector<uint8_t> overflow_pkt(audit_batch_header_bytes + 4U, 0);
    overflow_pkt[0] = 'A';
    overflow_pkt[1] = 'Y';
    overflow_pkt[2] = 'A';
    overflow_pkt[3] = 'U';
    overflow_pkt[4] = 1; // format = 1
    overflow_pkt[8] = 0x29;
    overflow_pkt[9] = 0x5C;
    overflow_pkt[10] = 0x8F;
    overflow_pkt[11] = 0x02; // count = 42949673
    std::vector<ayni_audit_record_v1> parsed;
    EXPECT_EQ(decode_audit_batch(overflow_pkt.data(), overflow_pkt.size(), parsed),
              AYNI_STORAGE_FAILED);
    overflow_pkt[8] = 0xFF;
    overflow_pkt[9] = 0xFF;
    overflow_pkt[10] = 0xFF;
    overflow_pkt[11] = 0xFF; // count = UINT32_MAX
    EXPECT_EQ(decode_audit_batch(overflow_pkt.data(), overflow_pkt.size(), parsed),
              AYNI_STORAGE_FAILED);
}

} // namespace
