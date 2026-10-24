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
    // Valid, but more than the owner's capacities hold: the log was written
    // under larger ones (C-LIMIT-007).
    no_room,
};

// batch is the identity of the batch the record is in.
using RecordHandler = RecordUse (*)(void *context, const BatchIdentity &batch,
                                    const RecordHeader &header, const uint8_t *body);

class PrefixScan {
  public:
    // The first batch scanned must carry first_commit_seq.
    explicit PrefixScan(uint64_t first_commit_seq) : next_commit_seq_(first_commit_seq) {}

    // Starts over at first_commit_seq. A scan that resumes after a snapshot
    // takes the largest transition_id and a writer_epoch from where it
    // resumes, the slot header or the activation batch a trimmed log starts
    // with, as if it had scanned the batches before (C-STORE-007). It counts
    // that writer_epoch as fenced: an older writer was isolated before that
    // one wrote, so its batches from there on are skipped.
    void reset(uint64_t first_commit_seq, uint64_t max_transition_id = 0,
               uint32_t writer_epoch = 0);
    // Checks every batch in bytes, which holds whole batches only, and passes
    // each record to handler in order. A batch must carry next_commit_seq()
    // and no smaller writer_epoch than the batches before it. Unknown record
    // flag bits, and unknown records marked REQUIRED, are unsupported; other
    // unknown records are skipped. Returns at the first problem, after which
    // the scan is not continued.
    //
    // A WRITER_FENCE batch takes the place of the next commit_seq and raises
    // the fence: a later batch of an older writer_epoch is one a fenced
    // writer stored after all, and is skipped without moving the prefix
    // (C-STORE-005). The scan handles WRITER_FENCE itself; handler never
    // sees it.
    //
    // Records reach handler as they are read, so a batch that fails later
    // has already shown its earlier records. Every failure ends recovery in
    // RESTRICTED, and the kernel then drops all it replayed, so no part of
    // a failed batch is ever visible (C-STORE-002, ADR-0026).
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
    // Batches of older writers are skipped; 0 while no fence was scanned
    // and reset() named none.
    uint32_t fenced_epoch_ = 0;
};

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_BATCH_PREFIX_SCAN_H
