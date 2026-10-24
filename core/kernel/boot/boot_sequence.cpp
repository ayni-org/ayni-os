#include "kernel/boot/boot_sequence.h"

#include "authority/grant/grant_records.h"
#include "durability/batch/batch_codec.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <array>
#include <cstring>
#include <limits>

namespace ayni::kernel {
namespace {

constexpr uint64_t recovery_boot_epoch = 0;
constexpr uint64_t no_deadline = std::numeric_limits<uint64_t>::max();
// The larger body of the two batches recovery appends.
constexpr uint32_t append_body_max = boot_epoch_record_length;
static_assert(durability::writer_fence_record_length <= append_body_max,
              "the WRITER_FENCE body fits the append body");
static_assert(durability::batch_size(append_body_max, 1) <= AYNI_STORAGE_BUFFER_BYTES_MIN,
              "recovery's batches must fit the smallest storage buffer");

} // namespace

BootSequence::BootSequence(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                           uint32_t buffer_size, WorkModel &model, WorkReplay &replay,
                           WriteSchedule &writes)
    : host_(host), completions_(completions), buffer_(buffer), buffer_size_(buffer_size),
      replay_(replay), writes_(writes), reader_(model, {}) {}

void BootSequence::reset(uint32_t isolated_epoch) {
    phase_ = Phase::discover;
    in_flight_ = false;
    retry_.reset();
    selected_ = {};
    activation_ = {};
    activation_seen_ = false;
    first_physical_ = true;
    cursor_ = log_bytes_ = payload_read_ = 0;
    known_durable_seq_ = 0;
    carry_ = payload_crc_ = 0;
    scan_.reset(1);
    last_boot_epoch_ = boot_epoch_ = 0;
    isolated_epoch_ = isolated_epoch;
    append_size_ = 0;
    boot_batch_ = {};
    restriction_ = AYNI_RESTRICTION_NONE;
}

void BootSequence::isolate(uint32_t minimum_epoch) {
    reset();
    fence_minimum_ = minimum_epoch;
    phase_ = Phase::pre_fence;
}

bool BootSequence::stopped() const {
    return phase_ == Phase::done || phase_ == Phase::restricted || phase_ == Phase::isolated;
}

bool BootSequence::mutates() const {
    return phase_ == Phase::fence || phase_ == Phase::pre_fence || phase_ == Phase::writer_record ||
           phase_ == Phase::boot_record;
}

bool BootSequence::wants_advance(uint64_t now) const {
    return !in_flight_ && !stopped() && (!mutates() || (writes_.ready(now) && retry_.ready(now)));
}

uint64_t BootSequence::next_deadline() const {
    if (in_flight_ || stopped()) {
        return no_deadline;
    }
    return mutates() ? storage_max(writes_.deadline(), retry_.deadline()) : 0U;
}

bool BootSequence::advance(uint64_t now) {
    if (!wants_advance(now) || !completions_.has_free_slot()) {
        return false;
    }
    if (mutates()) {
        writes_.started(now);
        retry_.started();
    }
    switch (completions_.issue(recovery_boot_epoch, &BootSequence::on_complete, this,
                               &BootSequence::call, this)) {
    case IssueResult::in_flight:
        in_flight_ = true;
        break;
    case IssueResult::rejected:
        // Only an append is retried.
        if (!mutates() || (phase_ == Phase::fence || phase_ == Phase::pre_fence) ||
            !retry_.failed(now)) {
            fail(AYNI_RESTRICTION_STORAGE_REJECTED);
        }
        break;
    case IssueResult::busy:
        return false;
    }
    return true;
}

BootSequence::Outcome BootSequence::outcome() const {
    if (phase_ == Phase::done) {
        return Outcome::epoch_durable;
    }
    if (phase_ == Phase::isolated) {
        return Outcome::writers_isolated;
    }
    return phase_ == Phase::restricted ? Outcome::restricted : Outcome::pending;
}

ayni_submit_result_t BootSequence::call(void *context, ayni_attempt_ref_v1 attempt,
                                        ayni_completion_t *completion) {
    return static_cast<BootSequence *>(context)->port(attempt, completion);
}

ayni_submit_result_t BootSequence::port(ayni_attempt_ref_v1 attempt,
                                        ayni_completion_t *completion) {
    switch (phase_) {
    case Phase::header:
        return host_.storage_read_snapshot(host_.ctx, activation_.slot, 0, buffer_,
                                           durability::snapshot_header_size, attempt, completion);
    case Phase::discover:
    case Phase::replay:
        return host_.storage_read_log(host_.ctx, cursor_, buffer_, buffer_size_, attempt,
                                      completion);
    case Phase::marker:
        return host_.storage_read_snapshot(
            host_.ctx, activation_.slot,
            durability::snapshot_header_size + selected_.payload_length, buffer_,
            durability::snapshot_marker_size, attempt, completion);
    // A page goes after the part of a record the last one cut.
    case Phase::payload:
        return host_.storage_read_snapshot(
            host_.ctx, activation_.slot, durability::snapshot_header_size + payload_read_,
            buffer_ + carry_,
            static_cast<uint32_t>(
                storage_min(buffer_size_ - carry_, selected_.payload_length - payload_read_)),
            attempt, completion);
    case Phase::pre_fence:
        return host_.storage_fence_writers(host_.ctx, fence_minimum_, attempt, completion);
    case Phase::fence:
        return host_.storage_fence_writers(host_.ctx, scan_.max_writer_epoch() + 1U, attempt,
                                           completion);
    case Phase::writer_record:
    case Phase::boot_record: {
        const durability::BatchIdentity id = next_batch();
        ayni_storage_batch_v1 batch{};
        batch.struct_size = sizeof(batch);
        batch.writer_epoch = id.writer_epoch;
        batch.transition_id = id.transition_id;
        batch.commit_seq = id.commit_seq;
        batch.encoded = {buffer_, append_size_};
        return host_.storage_append(host_.ctx, &batch, attempt, completion);
    }
    default:
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
}

void BootSequence::on_complete(void *context, const ayni_completion_result_v1 &result) {
    static_cast<BootSequence *>(context)->complete(result);
}

void BootSequence::complete(const ayni_completion_result_v1 &result) {
    in_flight_ = false;
    switch (phase_) {
    case Phase::fence:
    case Phase::pre_fence:
        fenced(result);
        break;
    case Phase::writer_record:
    case Phase::boot_record:
        appended(result);
        break;
    default:
        read(result);
        break;
    }
}

// The epoch installed must be above every writer the log holds, and fit a
// batch's writer_epoch; an isolate() fence must also reach its minimum.
// Anything else leaves the old writer possibly able to store.
void BootSequence::fenced(const ayni_completion_result_v1 &result) {
    if ((result.outcome != AYNI_FENCE_DRAINED && result.outcome != AYNI_FENCE_EPOCH_INSTALLED) ||
        result.value <= scan_.max_writer_epoch() || result.value > UINT32_MAX ||
        (phase_ == Phase::pre_fence && result.value < fence_minimum_)) {
        fail(AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
        return;
    }
    isolated_epoch_ = static_cast<uint32_t>(result.value);
    retry_.reset();
    if (phase_ == Phase::pre_fence) {
        phase_ = Phase::isolated;
        return;
    }
    phase_ = Phase::writer_record;
    prepare_append();
}

// A stored batch goes through the scan like one read, so the scan and the
// BOOT_EPOCH it holds move on to it.
void BootSequence::appended(const ayni_completion_result_v1 &result) {
    if (result.outcome != AYNI_WRITE_OK) {
        if (!retry_.failed(host_.monotonic_now_us(host_.ctx))) {
            fail(result.outcome == AYNI_WRITE_FAILED ? AYNI_RESTRICTION_STORAGE_WRITE_FAILED
                                                     : AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
        }
        return;
    }
    const bool is_boot = phase_ == Phase::boot_record;
    const durability::BatchIdentity stored = next_batch();
    check(scan_.scan(buffer_, append_size_, &BootSequence::on_record, this));
    if (phase_ == Phase::restricted) {
        return;
    }
    known_durable_seq_ = scan_.next_commit_seq() - 1U;
    log_bytes_ += append_size_;
    retry_.reset();
    if (is_boot) {
        boot_batch_ = stored;
        boot_epoch_ = last_boot_epoch_;
        phase_ = Phase::done;
    } else {
        phase_ = Phase::boot_record;
        prepare_append();
    }
}

// Only a log read may report the size of a batch too large for the buffer,
// and only that size means the state exceeds this boot's capacity; a count
// beyond the buffer means the Host wrote past it or misreports (ADR-0027
// decision 6).
void BootSequence::read(const ayni_completion_result_v1 &result) {
    if (result.outcome == AYNI_READ_BUFFER_TOO_SMALL) {
        const bool valid_requirement = (phase_ == Phase::discover || phase_ == Phase::replay) &&
                                       result.value > buffer_size_ && result.value <= UINT32_MAX;
        fail(valid_requirement ? AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY
                               : AYNI_RESTRICTION_STORAGE_READ_FAILED);
        return;
    }
    if (result.outcome != AYNI_WRITE_OK || result.value > buffer_size_) {
        fail(AYNI_RESTRICTION_STORAGE_READ_FAILED);
        return;
    }
    switch (phase_) {
    case Phase::header:
        if (result.value > durability::snapshot_header_size) {
            fail(AYNI_RESTRICTION_STORAGE_READ_FAILED);
            break;
        }
        check(durability::read_snapshot_header(buffer_, static_cast<uint32_t>(result.value),
                                               selected_));
        if (phase_ != Phase::restricted) {
            if (!durability::activation_matches(activation_, selected_)) {
                fail(AYNI_RESTRICTION_LOG_CORRUPT);
            } else {
                begin_replay();
            }
        }
        break;
    case Phase::discover:
    case Phase::replay:
        read_log(result);
        break;
    case Phase::marker:
        check(durability::read_snapshot_marker(buffer_, static_cast<uint32_t>(result.value),
                                               selected_));
        if (phase_ != Phase::restricted) {
            reader_.reset(selected_);
            if (selected_.payload_length == 0U) {
                check(reader_.complete());
                if (phase_ != Phase::restricted) {
                    phase_ = Phase::replay;
                }
            } else {
                phase_ = Phase::payload;
            }
        }
        break;
    case Phase::payload:
        if (result.value == 0U || result.value > buffer_size_ - carry_ ||
            result.value > selected_.payload_length - payload_read_) {
            fail(AYNI_RESTRICTION_LOG_CORRUPT);
            break;
        }
        read_payload(static_cast<uint32_t>(result.value));
        break;
    default:
        break;
    }
}

// Applies every whole record in the buffer and keeps the cut one for the
// next page. The payload ends on a record, with the CRC the header names.
void BootSequence::read_payload(uint32_t size) {
    payload_crc_ = durability::snapshot_crc32(buffer_ + carry_, size, payload_crc_);
    payload_read_ += size;
    const uint32_t end = carry_ + size;
    uint32_t at = 0;
    while (end - at >= durability::record_header_size) {
        const uint8_t *record = buffer_ + at;
        const durability::RecordHeader header = durability::read_record_header(record);
        // A record larger than the buffer can never be read whole.
        if (header.length > buffer_size_ - durability::record_header_size) {
            fail(AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
            return;
        }
        if (header.length > end - at - durability::record_header_size) {
            break;
        }
        check(reader_.apply(header, record + durability::record_header_size));
        if (phase_ == Phase::restricted) {
            return;
        }
        at += static_cast<uint32_t>(durability::record_header_size) + header.length;
    }
    carry_ = end - at;
    std::memmove(buffer_, buffer_ + at, carry_);
    if (payload_read_ == selected_.payload_length) {
        if (carry_ != 0U || payload_crc_ != selected_.payload_crc) {
            fail(AYNI_RESTRICTION_LOG_CORRUPT);
        } else {
            check(reader_.complete());
            if (phase_ != Phase::restricted) {
                phase_ = Phase::replay;
            }
        }
    }
}

// A read of 0 bytes is the end of the log. Discovery then goes to the
// activated slot, or straight to the replay when there is none.
void BootSequence::read_log(const ayni_completion_result_v1 &result) {
    if (result.value == 0U) {
        if (phase_ == Phase::discover) {
            if (activation_seen_) {
                phase_ = Phase::header;
            } else {
                begin_replay();
            }
        } else {
            after_replay();
        }
        return;
    }
    if (result.next_cursor <= cursor_) {
        fail(AYNI_RESTRICTION_STORAGE_READ_FAILED);
        return;
    }
    if (phase_ == Phase::discover) {
        log_bytes_ += result.value;
    }
    cursor_ = result.next_cursor;
    scan_bytes(static_cast<uint32_t>(result.value));
}

// The replay skips the batches the snapshot covers; discovery has already
// checked them.
void BootSequence::scan_bytes(uint32_t size) {
    uint32_t at = 0;
    while (at < size) {
        durability::BatchView batch{};
        const durability::BatchCheck parsed =
            durability::read_batch(buffer_ + at, size - at, batch);
        if (parsed != durability::BatchCheck::ok) {
            check(parsed);
            return;
        }
        if (phase_ == Phase::discover && first_physical_) {
            first_physical_ = false;
            if (batch.identity.commit_seq != 1U && !start_after_trim(batch)) {
                return;
            }
        }
        if (phase_ != Phase::replay || batch.identity.commit_seq > selected_.covers_commit_seq) {
            check(scan_.scan(buffer_ + at, batch.encoded_size, &BootSequence::on_record, this));
            if (phase_ == Phase::restricted) {
                return;
            }
            known_durable_seq_ = storage_max(known_durable_seq_, scan_.next_commit_seq() - 1U);
        }
        at += static_cast<uint32_t>(batch.encoded_size);
    }
}

// A log that does not start at commit_seq 1 was trimmed after an
// activation, so its first batch is that SNAPSHOT_COMMIT alone, and the scan
// continues from its boundary. A retained older activation may name a slot
// a later candidate already reused: only the latest one selects a slot.
bool BootSequence::start_after_trim(const durability::BatchView &batch) {
    durability::RecordReader records(batch);
    durability::RecordHeader header{};
    const uint8_t *body = nullptr;
    durability::SnapshotActivation initial{};
    if (records.next(header, body) != durability::RecordReader::Next::record ||
        batch.record_count != 1U || header.type != durability::snapshot_commit_record_type ||
        !durability::read_snapshot_activation(body, header.length, initial) ||
        batch.identity.commit_seq != initial.covers_commit_seq + 1U) {
        fail(AYNI_RESTRICTION_LOG_CORRUPT);
        return false;
    }
    scan_.reset(initial.covers_commit_seq + 1U, initial.max_transition_id,
                batch.identity.writer_epoch);
    return true;
}

// The replay reads the log from its start again; with a snapshot, the scan
// resumes after it and the slot's marker comes first.
void BootSequence::begin_replay() {
    cursor_ = 0;
    last_boot_epoch_ = 0;
    scan_.reset(1);
    if (activation_seen_) {
        scan_.reset(selected_.covers_commit_seq + 1U, selected_.max_transition_id,
                    selected_.writer_epoch);
        last_boot_epoch_ = selected_.boot_epoch;
        phase_ = Phase::marker;
    } else {
        phase_ = Phase::replay;
    }
}

// This boot needs a boot epoch, a transition_id and a writer epoch above
// what the log holds. A reload fenced already, with an epoch above the log.
void BootSequence::after_replay() {
    if (!replay_.complete() || !replay_.consistent(last_boot_epoch_)) {
        fail(AYNI_RESTRICTION_LOG_CORRUPT);
    } else if (!replay_.binding_matches()) {
        fail(AYNI_RESTRICTION_OWNER_BINDING_MISMATCH);
    } else if (last_boot_epoch_ >= foundation::max_boot_epoch) {
        fail(AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED);
    } else if (scan_.max_transition_id() > durability::transition_id_max) {
        fail(AYNI_RESTRICTION_LOG_CORRUPT);
    } else if (scan_.max_writer_epoch() == UINT32_MAX) {
        fail(AYNI_RESTRICTION_WRITER_EPOCH_EXHAUSTED);
    } else if (isolated_epoch_ != 0U) {
        if (isolated_epoch_ <= scan_.max_writer_epoch()) {
            fail(AYNI_RESTRICTION_LOG_CORRUPT);
        } else {
            phase_ = Phase::writer_record;
            prepare_append();
        }
    } else {
        phase_ = Phase::fence;
    }
}

// This boot's writer, right after what the log holds. The scan moves only
// once the batch is stored, so every attempt carries the same identity.
durability::BatchIdentity BootSequence::next_batch() const {
    return {isolated_epoch_, scan_.next_commit_seq(), scan_.max_transition_id() + 1U};
}

// WRITER_FENCE carries the writer epoch installed; BOOT_EPOCH one more than
// the largest boot epoch so far, so epochs only grow along the log.
void BootSequence::prepare_append() {
    std::array<uint8_t, append_body_max> body{};
    durability::RecordHeader header{};
    if (phase_ == Phase::boot_record) {
        header = {boot_epoch_record_type, boot_epoch_record_version, durability::record_required,
                  boot_epoch_record_length};
        foundation::store_le64(body.data(), last_boot_epoch_ + 1U);
    } else {
        header = {durability::writer_fence_record_type, durability::writer_fence_record_version,
                  durability::record_required, durability::writer_fence_record_length};
        foundation::store_le32(body.data(), isolated_epoch_);
    }
    durability::BatchWriter writer(buffer_, buffer_size_, next_batch());
    if (!writer.add_record(header, body.data())) {
        fail(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    append_size_ = static_cast<uint32_t>(writer.finish());
}

durability::RecordUse BootSequence::on_record(void *context, const durability::BatchIdentity &batch,
                                              const durability::RecordHeader &header,
                                              const uint8_t *body) {
    return static_cast<BootSequence *>(context)->record(batch, header, body);
}

// BOOT_EPOCH and SNAPSHOT_COMMIT are the sequence's own records; the scan
// checks WRITER_FENCE itself. Discovery classifies work headers even when
// the snapshot covers their bodies; replay applies uncovered records.
durability::RecordUse BootSequence::record(const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body) {
    if (header.type == boot_epoch_record_type) {
        if (header.version != boot_epoch_record_version) {
            return durability::RecordUse::unknown;
        }
        if (header.length != boot_epoch_record_length ||
            foundation::load_le64(body) <= last_boot_epoch_) {
            return durability::RecordUse::corrupt;
        }
        last_boot_epoch_ = foundation::load_le64(body);
        return durability::RecordUse::applied;
    }
    // An activation sits in the batch right after its snapshot, and each
    // one in the log has a larger generation than the one before.
    if (header.type == durability::snapshot_commit_record_type) {
        if (header.version != durability::snapshot_commit_record_version) {
            return durability::RecordUse::unknown;
        }
        durability::SnapshotActivation candidate{};
        if (!durability::read_snapshot_activation(body, header.length, candidate) ||
            batch.commit_seq != candidate.covers_commit_seq + 1U ||
            batch.transition_id != candidate.max_transition_id + 1U) {
            return durability::RecordUse::corrupt;
        }
        if (phase_ == Phase::discover) {
            if (activation_seen_ && candidate.generation <= activation_.generation) {
                return durability::RecordUse::corrupt;
            }
            activation_ = candidate;
            activation_seen_ = true;
        }
        return durability::RecordUse::applied;
    }
    if (phase_ == Phase::discover) {
        return WorkReplay::classify_header(header);
    }
    // A create can only use an ID allocated in the durable epoch preceding
    // this record. A later BOOT_EPOCH must not legitimize a future ID.
    uint64_t grant_id = 0;
    if (header.type == authority::grant_record_type &&
        (header.version == authority::grant_record_version ||
         header.version == authority::goal_grant_record_version) &&
        (!authority::read_grant_record_id(body, header.length, grant_id) ||
         foundation::id_boot_epoch(grant_id) != last_boot_epoch_)) {
        return durability::RecordUse::corrupt;
    }
    if (header.type == authority::grant_revocation_record_type &&
        (header.version == authority::grant_revocation_record_version ||
         header.version == authority::grant_revocation_with_work_record_version) &&
        (!authority::read_grant_record_id(body, header.length, grant_id) ||
         foundation::id_boot_epoch(grant_id) == 0U ||
         foundation::id_boot_epoch(grant_id) > last_boot_epoch_)) {
        return durability::RecordUse::corrupt;
    }
    return WorkReplay::on_record(&replay_, batch, header, body);
}

void BootSequence::fail(ayni_restriction_t reason) {
    phase_ = Phase::restricted;
    restriction_ = reason;
}

void BootSequence::check(durability::BatchCheck result) {
    switch (result) {
    case durability::BatchCheck::ok:
        break;
    case durability::BatchCheck::corrupt:
        fail(AYNI_RESTRICTION_LOG_CORRUPT);
        break;
    case durability::BatchCheck::unsupported:
        fail(AYNI_RESTRICTION_LOG_UNSUPPORTED);
        break;
    case durability::BatchCheck::no_room:
        fail(AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
        break;
    }
}

} // namespace ayni::kernel
