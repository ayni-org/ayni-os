#include "kernel/commit/commit_log.h"

namespace ayni::kernel {

CommitLog::CommitLog(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                     uint32_t buffer_size, WriteSchedule &writes)
    : host_(host), completions_(completions), writes_(writes), ring_(buffer, buffer_size) {}

void CommitLog::start(uint64_t boot_epoch, const durability::BatchIdentity &next_batch,
                      uint32_t batch_max) {
    ring_.clear();
    retry_.reset();
    failure_ = AYNI_RESTRICTION_NONE;
    in_flight_ = false;
    boot_epoch_ = boot_epoch;
    writer_epoch_ = next_batch.writer_epoch;
    transition_offset_ = next_batch.transition_id - next_batch.commit_seq;
    next_seq_ = next_batch.commit_seq;
    durable_seq_ = next_batch.commit_seq - 1U;
    batch_max_ = batch_max;
}

bool CommitLog::has_room() const {
    return failure_ == AYNI_RESTRICTION_NONE && !retry_.retrying() && ring_.fits(batch_max_);
}

durability::BatchWriter CommitLog::begin() {
    return durability::BatchWriter(ring_.begin_batch(batch_max_), batch_max_,
                                   identity_of(next_seq_));
}

void CommitLog::queue(std::size_t encoded_size) {
    ring_.end_batch(static_cast<uint32_t>(encoded_size));
    ++next_seq_;
}

bool CommitLog::advance(uint64_t now_us) {
    if (!wants_advance(now_us)) {
        return false;
    }
    if (!completions_.has_free_slot()) {
        return false;
    }
    retry_.started();
    writes_.started(now_us);
    switch (completions_.issue(boot_epoch_, &CommitLog::on_append, this, &CommitLog::call_append,
                               this)) {
    case IssueResult::in_flight:
        in_flight_ = true;
        return true;
    case IssueResult::rejected:
        if (!retry_.failed(now_us)) {
            failure_ = AYNI_RESTRICTION_STORAGE_REJECTED;
        }
        return true;
    case IssueResult::busy:
        break;
    }
    return false;
}

bool CommitLog::wants_advance(uint64_t now_us) const {
    return failure_ == AYNI_RESTRICTION_NONE && !in_flight_ && !ring_.empty() &&
           retry_.ready(now_us) && writes_.ready(now_us);
}

uint64_t CommitLog::next_deadline() const {
    if (failure_ != AYNI_RESTRICTION_NONE || in_flight_ || ring_.empty()) {
        return std::numeric_limits<uint64_t>::max();
    }
    return retry_.deadline() > writes_.deadline() ? retry_.deadline() : writes_.deadline();
}

ayni_submit_result_t CommitLog::call_append(void *context, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion) {
    const CommitLog &self = *static_cast<CommitLog *>(context);
    const durability::BatchIdentity identity = self.identity_of(self.durable_seq_ + 1U);
    uint32_t size = 0;
    const uint8_t *bytes = self.ring_.front(size);
    ayni_storage_batch_v1 batch{};
    batch.struct_size = sizeof(batch);
    batch.writer_epoch = identity.writer_epoch;
    batch.transition_id = identity.transition_id;
    batch.commit_seq = identity.commit_seq;
    batch.encoded.data = bytes;
    batch.encoded.size = size;
    return self.host_.storage_append(self.host_.ctx, &batch, attempt, completion);
}

void CommitLog::on_append(void *context, const ayni_completion_result_v1 &result) {
    static_cast<CommitLog *>(context)->finish_append(result);
}

void CommitLog::finish_append(const ayni_completion_result_v1 &result) {
    in_flight_ = false;
    switch (result.outcome) {
    case AYNI_WRITE_OK:
        ++durable_seq_;
        ring_.pop_front();
        retry_.reset();
        break;
    case AYNI_WRITE_FAILED:
        if (!retry_.failed(host_.monotonic_now_us(host_.ctx))) {
            failure_ = AYNI_RESTRICTION_STORAGE_WRITE_FAILED;
        }
        break;
    default:
        // UNKNOWN, and any undefined outcome: the batch may be stored.
        if (!retry_.failed(host_.monotonic_now_us(host_.ctx))) {
            failure_ = AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN;
        }
        break;
    }
}

durability::BatchIdentity CommitLog::identity_of(uint64_t commit_seq) const {
    return {writer_epoch_, commit_seq, commit_seq + transition_offset_};
}

} // namespace ayni::kernel
