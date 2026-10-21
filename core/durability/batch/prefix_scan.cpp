#include "durability/batch/prefix_scan.h"

#include "foundation/little_endian.h"

namespace ayni::durability {
namespace {

template <typename T> T larger(T a, T b) {
    return a > b ? a : b;
}

} // namespace

void PrefixScan::reset(uint64_t first_commit_seq, uint64_t max_transition_id,
                       uint32_t writer_epoch) {
    next_commit_seq_ = first_commit_seq;
    max_transition_id_ = max_transition_id;
    max_writer_epoch_ = writer_epoch;
    fenced_epoch_ = writer_epoch;
    saw_legacy_ = false;
}

BatchCheck PrefixScan::scan(const uint8_t *bytes, std::size_t size, RecordHandler handler,
                            void *context) {
    std::size_t offset = 0;
    while (offset < size) {
        BatchView batch{};
        BatchCheck check = read_batch(bytes + offset, size - offset, batch);
        if (check != BatchCheck::ok) {
            return check;
        }
        // A stale physical append cannot move the logical prefix past a
        // fence. Physical cursors still move past it in the Host.
        if (fenced_epoch_ != 0U && batch.identity.writer_epoch < fenced_epoch_) {
            offset += batch.encoded_size;
            continue;
        }
        saw_legacy_ = saw_legacy_ || batch.schema == 0U;
        if (batch.identity.commit_seq != next_commit_seq_ ||
            batch.identity.writer_epoch < max_writer_epoch_) {
            return BatchCheck::corrupt;
        }
        RecordReader records(batch);
        RecordHeader first{};
        const uint8_t *body = nullptr;
        const bool fence = records.next(first, body) == RecordReader::Next::record &&
                           first.type == writer_fence_record_type;
        if (fence) {
            if (first.version != 1U || first.flags != record_required) {
                return BatchCheck::unsupported;
            }
            if (batch.record_count != 1U || batch.payload_length != record_header_size + 4U ||
                first.length != 4U || foundation::load_le32(body) != batch.identity.writer_epoch ||
                batch.identity.writer_epoch <= max_writer_epoch_) {
                return BatchCheck::corrupt;
            }
            fenced_epoch_ = batch.identity.writer_epoch;
            check = BatchCheck::ok;
        } else {
            check = scan_records(batch, handler, context);
        }
        if (check != BatchCheck::ok) {
            return check;
        }
        ++next_commit_seq_;
        max_transition_id_ = larger(max_transition_id_, batch.identity.transition_id);
        max_writer_epoch_ = larger(max_writer_epoch_, batch.identity.writer_epoch);
        offset += batch.encoded_size;
    }
    return BatchCheck::ok;
}

// Unknown record flag bits, like unknown batch flags, may carry meaning this
// reader cannot honor (C-STORE-004).
BatchCheck PrefixScan::scan_records(const BatchView &batch, RecordHandler handler, void *context) {
    RecordReader reader(batch);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    for (;;) {
        switch (reader.next(header, body)) {
        case RecordReader::Next::record:
            break;
        case RecordReader::Next::end:
            return BatchCheck::ok;
        case RecordReader::Next::corrupt:
            return BatchCheck::corrupt;
        }
        if ((header.flags & ~record_required) != 0U) {
            return BatchCheck::unsupported;
        }
        if (header.type == writer_fence_record_type ||
            (header.type == snapshot_commit_record_type &&
             (batch.record_count != 1U || header.flags != record_required))) {
            return BatchCheck::corrupt;
        }
        switch (handler(context, batch.identity, header, body)) {
        case RecordUse::applied:
            break;
        case RecordUse::unknown:
            if ((header.flags & record_required) != 0U) {
                return BatchCheck::unsupported;
            }
            break;
        case RecordUse::corrupt:
            return BatchCheck::corrupt;
        case RecordUse::no_room:
            return BatchCheck::no_room;
        }
    }
}

} // namespace ayni::durability
