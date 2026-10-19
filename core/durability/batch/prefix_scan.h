// Checks the batches of the log prefix in log order and hands each record to
// the module that owns its type (C-STORE-003, C-STORE-004, C-STORE-009,
// ADR-0016). The Host returns only whole batches, so every batch scanned
// belongs to the committed region: damage or a commit_seq gap is corruption,
// never a torn tail to skip.

#ifndef AYNI_CORE_DURABILITY_BATCH_PREFIX_SCAN_H
#define AYNI_CORE_DURABILITY_BATCH_PREFIX_SCAN_H

#include "durability/batch/batch_codec.h"

#include <cstddef>
#include <cstdint>

namespace ayni::durability {

// What a record's owner made of it.
enum class RecordUse {
    applied, // a type and version the owner knows, with a valid body
    unknown, // not a type and version the owner knows
    corrupt, // known, but the body breaks the owner's rules
};

using RecordHandler = RecordUse (*)(void *context, const RecordHeader &header, const uint8_t *body);

class PrefixScan {
  public:
    // The first batch scanned must carry first_commit_seq.
    explicit PrefixScan(uint64_t first_commit_seq) : next_commit_seq_(first_commit_seq) {}

    // Checks every batch in bytes, which holds whole batches only, and passes
    // each record to handler in order. Unknown record flag bits, and unknown
    // records marked REQUIRED, are unsupported; other unknown records are
    // skipped. Returns at the first problem, after which the scan is not
    // continued.
    BatchCheck scan(const uint8_t *bytes, std::size_t size, RecordHandler handler, void *context);

    // The commit_seq the next batch must carry.
    uint64_t next_commit_seq() const { return next_commit_seq_; }
    // Largest values in the batches scanned so far; 0 before the first.
    uint64_t max_transition_id() const { return max_transition_id_; }
    uint32_t max_writer_epoch() const { return max_writer_epoch_; }

  private:
    static BatchCheck scan_records(const BatchView &batch, RecordHandler handler, void *context);

    uint64_t next_commit_seq_;
    uint64_t max_transition_id_ = 0;
    uint32_t max_writer_epoch_ = 0;
};

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_BATCH_PREFIX_SCAN_H
