#include "kernel/storage/storage_maintenance.h"

#include <array>
#include <cstring>
#include <limits>

namespace ayni::kernel {

StorageMaintenance::StorageMaintenance(const ayni_host_v1 &host, CompletionPool &completions,
                                       uint8_t *buffer, uint32_t capacity, WorkModel &model,
                                       WriteSchedule &writes)
    : host_(host), completions_(completions), buffer_(buffer), capacity_(capacity), writes_(writes),
      writer_(model) {}

void StorageMaintenance::adopt(uint64_t generation, uint32_t slot) {
    generation_ = generation;
    active_slot_ = slot;
}

bool StorageMaintenance::generation_exhausted() const {
    return generation_ == std::numeric_limits<uint64_t>::max();
}

// A migration's snapshot is the first of schema 1, so it is generation 1 in
// slot 0 whatever the schema 0 log held. Otherwise the candidate takes the
// slot not in use, which recovery ignores until the activation names it.
void StorageMaintenance::start(const SnapshotBoundary &boundary, uint32_t writer_epoch,
                               bool migration) {
    const SnapshotLayout layout = writer_.measure();
    metadata_ = durability::SnapshotMetadata{};
    metadata_.generation = migration ? 1U : generation_ + 1U;
    metadata_.covers_commit_seq = boundary.covers_commit_seq;
    metadata_.max_transition_id = boundary.max_transition_id;
    metadata_.boot_epoch = boundary.boot_epoch;
    metadata_.writer_epoch = boundary.writer_epoch;
    metadata_.payload_length = layout.payload_length;
    metadata_.record_count = layout.record_count;
    slot_ = migration || generation_ == 0U ? 0U : 1U - active_slot_;
    writer_epoch_ = writer_epoch;
    migration_ = migration;
    // The activation is the batch right after the boundary (records.md).
    activation_batch_ = {writer_epoch, boundary.covers_commit_seq + 1U,
                         boundary.max_transition_id + 1U};
    writer_.reset();
    retry_.reset();
    in_flight_ = false;
    offset_ = 0;
    page_size_ = 0;
    crc_ = 0;
    failure_ = AYNI_RESTRICTION_NONE;
    phase_ = Phase::checksum;
    mode_ = migration ? Mode::migration : Mode::runtime;
}

bool StorageMaintenance::active() const {
    return phase_ != Phase::idle && phase_ != Phase::done && phase_ != Phase::failed;
}

bool StorageMaintenance::done() const {
    return phase_ == Phase::done;
}

bool StorageMaintenance::mutates() const {
    return phase_ == Phase::header || phase_ == Phase::payload || phase_ == Phase::marker ||
           phase_ == Phase::activation || phase_ == Phase::trim;
}

bool StorageMaintenance::wants_advance(uint64_t now) const {
    return active() && !in_flight_ && (!mutates() || (writes_.ready(now) && retry_.ready(now)));
}

uint64_t StorageMaintenance::next_deadline() const {
    if (!active() || in_flight_) {
        return std::numeric_limits<uint64_t>::max();
    }
    return mutates() ? storage_max(writes_.deadline(), retry_.deadline()) : 0U;
}

bool StorageMaintenance::advance(uint64_t now) {
    if (!wants_advance(now)) {
        return false;
    }
    // The header carries the payload CRC, and the payload is written after
    // it, so a first pass over the model computes the CRC. The model does
    // not change in between (ADR-0027 decision 4).
    if (phase_ == Phase::checksum) {
        switch (writer_.next(buffer_, capacity_, page_size_)) {
        case WorkSnapshotWriter::Page::page:
            crc_ = durability::snapshot_crc32(buffer_, page_size_, crc_);
            offset_ += page_size_;
            break;
        case WorkSnapshotWriter::Page::done:
            if (offset_ != metadata_.payload_length) {
                fail(AYNI_RESTRICTION_INTERNAL);
            } else {
                metadata_.payload_crc = crc_;
                writer_.reset();
                offset_ = 0;
                phase_ = Phase::header;
                prepare_header();
            }
            break;
        case WorkSnapshotWriter::Page::no_room:
            // The buffer holds the largest record (ayni_create checks it).
            fail(AYNI_RESTRICTION_INTERNAL);
            break;
        }
        return true;
    }
    // A request waiting for a completion slot has not made an attempt.
    if (!completions_.has_free_slot()) {
        return false;
    }
    if (mutates()) {
        writes_.started(now);
        retry_.started();
    }
    // A migration writes before this boot's epoch is durable, so its
    // requests carry boot_epoch 0, as the boot sequence's do (C-ID-006).
    switch (completions_.issue(migration_ ? 0U : metadata_.boot_epoch,
                               &StorageMaintenance::on_complete, this, &StorageMaintenance::call,
                               this)) {
    case IssueResult::in_flight:
        in_flight_ = true;
        break;
    case IssueResult::rejected:
        if (!mutates() || !retry_.failed(now)) {
            fail(AYNI_RESTRICTION_STORAGE_REJECTED);
        }
        break;
    case IssueResult::busy:
        return false;
    }
    return true;
}

ayni_submit_result_t StorageMaintenance::call(void *context, ayni_attempt_ref_v1 attempt,
                                              ayni_completion_t *completion) {
    return static_cast<StorageMaintenance *>(context)->port(attempt, completion);
}

ayni_submit_result_t StorageMaintenance::port(ayni_attempt_ref_v1 attempt,
                                              ayni_completion_t *completion) {
    switch (phase_) {
    case Phase::header:
    case Phase::payload:
    case Phase::marker:
        return host_.storage_write_snapshot(host_.ctx, writer_epoch_, slot_, offset_,
                                            {buffer_, page_size_}, attempt, completion);
    case Phase::verify_header:
        return host_.storage_read_snapshot(host_.ctx, slot_, 0, buffer_,
                                           durability::snapshot_header_size, attempt, completion);
    case Phase::verify_payload:
        return host_.storage_read_snapshot(
            host_.ctx, slot_, durability::snapshot_header_size + offset_, buffer_,
            static_cast<uint32_t>(storage_min(capacity_, metadata_.payload_length - offset_)),
            attempt, completion);
    case Phase::activation: {
        ayni_storage_batch_v1 batch{};
        batch.struct_size = sizeof(batch);
        batch.writer_epoch = activation_batch_.writer_epoch;
        batch.transition_id = activation_batch_.transition_id;
        batch.commit_seq = activation_batch_.commit_seq;
        batch.encoded = {buffer_, page_size_};
        return host_.storage_append(host_.ctx, &batch, attempt, completion);
    }
    case Phase::trim:
        return host_.storage_trim_prefix(host_.ctx, writer_epoch_, activation_batch_.transition_id,
                                         activation_batch_.commit_seq, attempt, completion);
    default:
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
}

void StorageMaintenance::on_complete(void *context, const ayni_completion_result_v1 &result) {
    static_cast<StorageMaintenance *>(context)->complete(result);
}

// A writing request that failed or may have been stored is retried
// unchanged (AppendRetry); the Host takes a repeat as the same write
// (ayni_host_v1). A read that fails ends the run.
void StorageMaintenance::complete(const ayni_completion_result_v1 &result) {
    in_flight_ = false;
    if (result.outcome != AYNI_WRITE_OK) {
        if (!mutates() || !retry_.failed(host_.monotonic_now_us(host_.ctx))) {
            fail(mutates() ? (result.outcome == AYNI_WRITE_FAILED
                                  ? AYNI_RESTRICTION_STORAGE_WRITE_FAILED
                                  : AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN)
                           : AYNI_RESTRICTION_STORAGE_READ_FAILED);
        }
        return;
    }
    retry_.reset();
    switch (phase_) {
    case Phase::header:
        offset_ = durability::snapshot_header_size;
        prepare_payload();
        break;
    case Phase::payload:
        offset_ += page_size_;
        prepare_payload();
        break;
    // The read-back proves the slot holds what was written before the
    // marker vouches for it (C-STORE-006); a mismatch is damage.
    case Phase::verify_header: {
        std::array<uint8_t, durability::snapshot_header_size> expected{};
        durability::write_snapshot_header(metadata_, expected.data());
        if (result.value != expected.size() ||
            std::memcmp(buffer_, expected.data(), expected.size()) != 0) {
            fail(AYNI_RESTRICTION_LOG_CORRUPT);
            break;
        }
        offset_ = 0;
        crc_ = 0;
        if (metadata_.payload_length == 0U) {
            prepare_marker();
        } else {
            phase_ = Phase::verify_payload;
        }
        break;
    }
    case Phase::verify_payload:
        // An empty or oversized read is the Host's fault, not the slot's.
        if (result.value == 0U || result.value > capacity_ ||
            result.value > metadata_.payload_length - offset_) {
            fail(AYNI_RESTRICTION_STORAGE_READ_FAILED);
            break;
        }
        crc_ = durability::snapshot_crc32(buffer_, static_cast<uint32_t>(result.value), crc_);
        offset_ += result.value;
        if (offset_ == metadata_.payload_length) {
            if (crc_ != metadata_.payload_crc) {
                fail(AYNI_RESTRICTION_LOG_CORRUPT);
            } else {
                prepare_marker();
            }
        }
        break;
    case Phase::marker:
        prepare_activation();
        break;
    case Phase::activation:
        phase_ = Phase::trim;
        break;
    case Phase::trim:
        phase_ = Phase::done;
        break;
    default:
        fail(AYNI_RESTRICTION_INTERNAL);
        break;
    }
}

void StorageMaintenance::prepare_header() {
    durability::write_snapshot_header(metadata_, buffer_);
    page_size_ = durability::snapshot_header_size;
}

// The second pass writes the pages the first one measured; ending at any
// other length means the model changed, a defect.
void StorageMaintenance::prepare_payload() {
    switch (writer_.next(buffer_, capacity_, page_size_)) {
    case WorkSnapshotWriter::Page::page:
        phase_ = Phase::payload;
        break;
    case WorkSnapshotWriter::Page::done:
        if (offset_ != durability::snapshot_header_size + metadata_.payload_length) {
            fail(AYNI_RESTRICTION_INTERNAL);
        } else {
            phase_ = Phase::verify_header;
        }
        break;
    case WorkSnapshotWriter::Page::no_room:
        fail(AYNI_RESTRICTION_INTERNAL);
        break;
    }
}

void StorageMaintenance::prepare_marker() {
    phase_ = Phase::marker;
    offset_ = durability::snapshot_header_size + metadata_.payload_length;
    durability::write_snapshot_marker(metadata_, buffer_);
    page_size_ = durability::snapshot_marker_size;
}

// A migration's activation keeps the schema 0 batch header that a reader of
// the old schema can parse: it then meets a REQUIRED record it does not
// know and stops, instead of running on the stale log (ADR-0027 decision 11).
void StorageMaintenance::prepare_activation() {
    std::array<uint8_t, durability::snapshot_commit_record_length> body{};
    durability::write_snapshot_activation({slot_, metadata_.payload_crc, metadata_.generation,
                                           metadata_.covers_commit_seq,
                                           metadata_.max_transition_id},
                                          body.data());
    durability::BatchWriter writer(buffer_, capacity_, activation_batch_,
                                   migration_ ? 0U : durability::storage_schema);
    if (!writer.add_record({durability::snapshot_commit_record_type,
                            durability::snapshot_commit_record_version, durability::record_required,
                            durability::snapshot_commit_record_length},
                           body.data())) {
        fail(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    page_size_ = static_cast<uint32_t>(writer.finish());
    phase_ = Phase::activation;
}

void StorageMaintenance::fail(ayni_restriction_t reason) {
    failure_ = reason;
    phase_ = Phase::failed;
}

} // namespace ayni::kernel
