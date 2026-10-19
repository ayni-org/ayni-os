#include "durability/batch/prefix_scan.h"

namespace ayni::durability {
namespace {

template <typename T> T larger(T a, T b) {
    return a > b ? a : b;
}

} // namespace

BatchCheck PrefixScan::scan(const uint8_t *bytes, std::size_t size, RecordHandler handler,
                            void *context) {
    std::size_t offset = 0;
    while (offset < size) {
        BatchView batch{};
        BatchCheck check = read_batch(bytes + offset, size - offset, batch);
        if (check != BatchCheck::ok) {
            return check;
        }
        if (batch.identity.commit_seq != next_commit_seq_) {
            return BatchCheck::corrupt;
        }
        check = scan_records(batch, handler, context);
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
        switch (handler(context, header, body)) {
        case RecordUse::applied:
            break;
        case RecordUse::unknown:
            if ((header.flags & record_required) != 0U) {
                return BatchCheck::unsupported;
            }
            break;
        case RecordUse::corrupt:
            return BatchCheck::corrupt;
        }
    }
}

} // namespace ayni::durability
