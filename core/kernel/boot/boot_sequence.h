// Recovery from ayni_start up to RUNNING (ADR-0016). Reads the whole log
// prefix, which durability checks batch by batch, then persists this boot's
// epoch in a batch of its own before the kernel opens any output (C-ID-009).
// Replaying domain records joins as those records are introduced (MA-010).
//
// One storage request is in flight at a time, so the kernel's storage buffer
// serves every read and then the append. Requests use boot_epoch 0: this
// boot's epoch is not durable until the append completes (C-ID-006).

#ifndef AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H
#define AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H

#include "durability/batch/prefix_scan.h"
#include "kernel/completion/completion_pool.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// BOOT_EPOCH (contracts/storage/records.md): a REQUIRED record whose body is
// the epoch as a little-endian uint64.
constexpr uint16_t boot_epoch_record_type = 1;
constexpr uint16_t boot_epoch_record_version = 1;
constexpr uint32_t boot_epoch_record_length = 8;

class BootSequence {
  public:
    enum class Outcome {
        pending,
        epoch_durable, // RUNNING may start; boot_epoch() is set
        restricted,    // restriction() says why
    };

    // host has both storage ports, and it, completions and buffer outlive
    // the sequence. buffer_size is within the ayni.h storage buffer range.
    BootSequence(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                 uint32_t buffer_size);

    BootSequence(const BootSequence &) = delete;
    BootSequence &operator=(const BootSequence &) = delete;

    // Owner thread. Issues the next storage request when one is due and
    // returns whether it called the Host.
    bool advance();

    // A request is due: advance() would call the Host.
    bool wants_advance() const;

    Outcome outcome() const;
    ayni_restriction_t restriction() const { return restriction_; }
    // 0 until the BOOT_EPOCH batch is durable.
    uint64_t boot_epoch() const { return boot_epoch_; }

  private:
    enum class Phase { read_due, reading, append_due, appending, done, restricted };

    static ayni_submit_result_t call_read(void *context, ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion);
    static ayni_submit_result_t call_append(void *context, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion);
    static void on_read(void *context, const ayni_completion_result_v1 &result);
    static void on_append(void *context, const ayni_completion_result_v1 &result);
    static durability::RecordUse on_record(void *context, const durability::RecordHeader &header,
                                           const uint8_t *body);

    bool issue(Phase in_flight, CompletionHandler handler, PortCall call);
    void finish_read(const ayni_completion_result_v1 &result);
    void finish_append(const ayni_completion_result_v1 &result);
    durability::RecordUse apply_record(const durability::RecordHeader &header, const uint8_t *body);
    void enter_restricted(ayni_restriction_t reason);

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    uint8_t *buffer_;
    uint32_t buffer_size_;
    Phase phase_ = Phase::read_due;
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
    // Without snapshots (MA-010) the log starts at commit_seq 1.
    durability::PrefixScan scan_{1};
    uint64_t last_boot_epoch_ = 0; // the largest in the log so far
    uint64_t boot_epoch_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H
