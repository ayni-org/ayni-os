// The log writer of a RUNNING kernel (C-COMMIT-004, C-STORE-001, ADR-0026).
// Every transition encodes one batch into the journal ring in the storage
// buffer as it applies, and the log appends the queued batches to the Host
// in commit order, one request in flight at a time. durable_seq is the
// commit_seq of the last batch the Host stored; a batch above it can still
// be lost, and a later boot then never sees its transition.
//
// A transition needs room for the largest batch any transition writes
// before it starts, so encoding never fails halfway: with room, the kernel
// applies it; without, it waits until stored batches leave the ring. A
// failed or unknown append ends the writer, because a batch after a gap
// could never be read in order: the kernel becomes RESTRICTED.

#ifndef AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H
#define AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H

#include "durability/batch/batch_codec.h"
#include "durability/journal/journal_ring.h"
#include "kernel/completion/completion_pool.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

class CommitLog {
  public:
    // host has storage_append, and it, completions and buffer outlive the
    // log.
    CommitLog(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
              uint32_t buffer_size);

    CommitLog(const CommitLog &) = delete;
    CommitLog &operator=(const CommitLog &) = delete;

    // Entering RUNNING, once recovery no longer uses the buffer: the next
    // batch is next_batch, the batches before it are durable, and no batch
    // is larger than batch_max, which fits the buffer with
    // JournalRing::entry_overhead. Requests carry boot_epoch.
    void start(uint64_t boot_epoch, const durability::BatchIdentity &next_batch,
               uint32_t batch_max);

    // Whether the next transition may start: the writer has not failed and
    // the ring holds a batch of batch_max bytes in one piece.
    bool has_room() const;
    // Begins the batch of the next transition, which takes the next
    // identity; only when has_room().
    durability::BatchWriter begin();
    // Queues the batch begin() began, encoded_size bytes from finish().
    void queue(std::size_t encoded_size);

    // Owner thread. Appends the oldest queued batch when none is in flight
    // and returns whether it called the Host.
    bool advance();
    // A batch is queued, none is in flight and the writer has not failed.
    bool wants_advance() const;
    // Nothing queued and nothing in flight.
    bool idle() const { return !in_flight_ && ring_.empty(); }

    uint64_t durable_seq() const { return durable_seq_; }
    // Why the writer stopped; AYNI_RESTRICTION_NONE while it works.
    ayni_restriction_t failure() const { return failure_; }

  private:
    static ayni_submit_result_t call_append(void *context, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion);
    static void on_append(void *context, const ayni_completion_result_v1 &result);
    void finish_append(const ayni_completion_result_v1 &result);
    // The identity of the batch with commit_seq.
    durability::BatchIdentity identity_of(uint64_t commit_seq) const;

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    durability::JournalRing ring_;
    uint64_t boot_epoch_ = 0;
    uint32_t writer_epoch_ = 0;
    // transition_id - commit_seq, modulo 2^64, the same for every batch of a
    // boot: each transition takes the next of both.
    uint64_t transition_offset_ = 0;
    uint64_t next_seq_ = 0;    // of the batch begin() begins next
    uint64_t durable_seq_ = 0; // of the last batch stored
    uint32_t batch_max_ = 0;
    bool in_flight_ = false;
    ayni_restriction_t failure_ = AYNI_RESTRICTION_NONE;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H
