#include "kernel/boot/boot_sequence.h"

#include "durability/batch/batch_codec.h"
#include "foundation/little_endian.h"

#include <array>
#include <limits>

namespace ayni::kernel {
namespace {

constexpr uint64_t recovery_boot_epoch = 0;
constexpr std::size_t boot_epoch_batch_size = durability::batch_size(boot_epoch_record_length, 1);
static_assert(boot_epoch_batch_size <= AYNI_STORAGE_BUFFER_BYTES_MIN,
              "the BOOT_EPOCH batch must fit the smallest storage buffer");

} // namespace

BootSequence::BootSequence(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                           uint32_t buffer_size)
    : host_(host), completions_(completions), buffer_(buffer), buffer_size_(buffer_size) {}

bool BootSequence::advance() {
    switch (phase_) {
    case Phase::read_due:
        return issue(Phase::reading, &BootSequence::on_read, &BootSequence::call_read);
    case Phase::append_due:
        return issue(Phase::appending, &BootSequence::on_append, &BootSequence::call_append);
    case Phase::reading:
    case Phase::appending:
    case Phase::done:
    case Phase::restricted:
        break;
    }
    return false;
}

bool BootSequence::wants_advance() const {
    return phase_ == Phase::read_due || phase_ == Phase::append_due;
}

BootSequence::Outcome BootSequence::outcome() const {
    switch (phase_) {
    case Phase::done:
        return Outcome::epoch_durable;
    case Phase::restricted:
        return Outcome::restricted;
    case Phase::read_due:
    case Phase::reading:
    case Phase::append_due:
    case Phase::appending:
        break;
    }
    return Outcome::pending;
}

bool BootSequence::issue(Phase in_flight, CompletionHandler handler, PortCall call) {
    switch (completions_.issue(recovery_boot_epoch, handler, this, call, this)) {
    case IssueResult::in_flight:
        phase_ = in_flight;
        return true;
    case IssueResult::rejected:
        enter_restricted(AYNI_RESTRICTION_STORAGE_REJECTED);
        return true;
    case IssueResult::busy:
        // Only reachable if something else holds every slot; the sequence
        // has at most one request in flight. It retries on the next step.
        break;
    }
    return false;
}

ayni_submit_result_t BootSequence::call_read(void *context, ayni_attempt_ref_v1 attempt,
                                             ayni_completion_t *completion) {
    const BootSequence &self = *static_cast<BootSequence *>(context);
    return self.host_.storage_read_prefix(self.host_.ctx, self.scan_.next_commit_seq(),
                                          self.buffer_, self.buffer_size_, attempt, completion);
}

ayni_submit_result_t BootSequence::call_append(void *context, ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    const BootSequence &self = *static_cast<BootSequence *>(context);
    // Writer fencing (MA-010) is what changes writer_epoch; an empty log
    // starts at 1.
    const uint32_t max_writer_epoch = self.scan_.max_writer_epoch();
    const durability::BatchIdentity identity{max_writer_epoch == 0U ? 1U : max_writer_epoch,
                                             self.scan_.next_commit_seq(),
                                             self.scan_.max_transition_id() + 1U};
    std::array<uint8_t, boot_epoch_record_length> body{};
    foundation::store_le64(body.data(), self.last_boot_epoch_ + 1U);

    durability::BatchWriter writer(self.buffer_, self.buffer_size_, identity);
    // The record fits (static_assert above), and finish() cannot fail after
    // a record was added.
    static_cast<void>(writer.add_record({boot_epoch_record_type, boot_epoch_record_version,
                                         durability::record_required, boot_epoch_record_length},
                                        body.data()));
    const std::size_t encoded_size = writer.finish();

    ayni_storage_batch_v1 batch{};
    batch.struct_size = sizeof(batch);
    batch.writer_epoch = identity.writer_epoch;
    batch.transition_id = identity.transition_id;
    batch.commit_seq = identity.commit_seq;
    batch.encoded.data = self.buffer_;
    batch.encoded.size = static_cast<uint32_t>(encoded_size);
    return self.host_.storage_append(self.host_.ctx, &batch, attempt, completion);
}

void BootSequence::on_read(void *context, const ayni_completion_result_v1 &result) {
    static_cast<BootSequence *>(context)->finish_read(result);
}

void BootSequence::on_append(void *context, const ayni_completion_result_v1 &result) {
    static_cast<BootSequence *>(context)->finish_append(result);
}

void BootSequence::finish_read(const ayni_completion_result_v1 &result) {
    // A count beyond the buffer means the Host wrote past it or misreports;
    // either way the content is not reliable.
    if (result.outcome != AYNI_WRITE_OK || result.value > buffer_size_) {
        enter_restricted(AYNI_RESTRICTION_STORAGE_READ_FAILED);
        return;
    }
    if (result.value == 0U) {
        if (last_boot_epoch_ == std::numeric_limits<uint64_t>::max()) {
            enter_restricted(AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED);
        } else {
            phase_ = Phase::append_due;
        }
        return;
    }
    switch (scan_.scan(buffer_, static_cast<std::size_t>(result.value), &BootSequence::on_record,
                       this)) {
    case durability::BatchCheck::ok:
        phase_ = Phase::read_due;
        break;
    case durability::BatchCheck::corrupt:
        enter_restricted(AYNI_RESTRICTION_LOG_CORRUPT);
        break;
    case durability::BatchCheck::unsupported:
        enter_restricted(AYNI_RESTRICTION_LOG_UNSUPPORTED);
        break;
    }
}

void BootSequence::finish_append(const ayni_completion_result_v1 &result) {
    switch (result.outcome) {
    case AYNI_WRITE_OK:
        boot_epoch_ = last_boot_epoch_ + 1U;
        phase_ = Phase::done;
        break;
    case AYNI_WRITE_FAILED:
        enter_restricted(AYNI_RESTRICTION_STORAGE_WRITE_FAILED);
        break;
    default:
        // UNKNOWN, and any undefined outcome: the batch may be stored.
        enter_restricted(AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
        break;
    }
}

durability::RecordUse BootSequence::on_record(void *context, const durability::RecordHeader &header,
                                              const uint8_t *body) {
    return static_cast<BootSequence *>(context)->apply_record(header, body);
}

// Each boot writes one more than the largest epoch before it, so epochs only
// grow along the log; 0 is never written.
durability::RecordUse BootSequence::apply_record(const durability::RecordHeader &header,
                                                 const uint8_t *body) {
    if (header.type != boot_epoch_record_type || header.version != boot_epoch_record_version) {
        return durability::RecordUse::unknown;
    }
    if (header.length != boot_epoch_record_length) {
        return durability::RecordUse::corrupt;
    }
    const uint64_t epoch = foundation::load_le64(body);
    if (epoch <= last_boot_epoch_) {
        return durability::RecordUse::corrupt;
    }
    last_boot_epoch_ = epoch;
    return durability::RecordUse::applied;
}

void BootSequence::enter_restricted(ayni_restriction_t reason) {
    phase_ = Phase::restricted;
    restriction_ = reason;
}

} // namespace ayni::kernel
