#include "kernel/boot/boot_sequence.h"

#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <array>
#include <cstring>
#include <limits>

namespace ayni::kernel {
namespace {
constexpr uint64_t max_logged_transition_id = (uint64_t{1} << 63U) - 1U;
constexpr uint64_t no_deadline = std::numeric_limits<uint64_t>::max();
} // namespace

bool read_activation(const uint8_t *body, uint32_t size, SnapshotActivation &out) {
    if (size != activation_record_length) {
        return false;
    }
    out = {foundation::load_le32(body), foundation::load_le32(body + 4),
           foundation::load_le64(body + 8), foundation::load_le64(body + 16),
           foundation::load_le64(body + 24)};
    return out.slot < 2U && out.generation != 0U &&
           out.max_transition_id <= max_logged_transition_id &&
           out.covers_commit_seq <= out.max_transition_id;
}

void write_activation(const SnapshotActivation &a, uint8_t *body) {
    foundation::store_le32(body, a.slot);
    foundation::store_le32(body + 4, a.payload_crc);
    foundation::store_le64(body + 8, a.generation);
    foundation::store_le64(body + 16, a.covers_commit_seq);
    foundation::store_le64(body + 24, a.max_transition_id);
}

bool activation_matches(const SnapshotActivation &a, const durability::SnapshotMetadata &m) {
    return a.generation == m.generation && a.covers_commit_seq == m.covers_commit_seq &&
           a.max_transition_id == m.max_transition_id && a.payload_crc == m.payload_crc;
}

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
    legacy_ = false;
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

bool BootSequence::mutates() const {
    return phase_ == Phase::fence || phase_ == Phase::pre_fence || phase_ == Phase::writer_record ||
           phase_ == Phase::boot_record;
}

bool BootSequence::wants_advance(uint64_t now) const {
    return !in_flight_ && phase_ != Phase::done && phase_ != Phase::restricted &&
           phase_ != Phase::migration && phase_ != Phase::isolated &&
           (!mutates() || (writes_.ready(now) && retry_.ready(now)));
}

uint64_t BootSequence::next_deadline() const {
    if (in_flight_ || phase_ == Phase::done || phase_ == Phase::restricted ||
        phase_ == Phase::migration || phase_ == Phase::isolated) {
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
    switch (completions_.issue(0, &BootSequence::on_complete, this, &BootSequence::call, this)) {
    case IssueResult::in_flight:
        in_flight_ = true;
        break;
    case IssueResult::rejected:
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
    if (phase_ == Phase::migration) {
        return Outcome::migration_ready;
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
        const durability::BatchIdentity id{isolated_epoch_, scan_.next_commit_seq(),
                                           scan_.max_transition_id() + 1U};
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
    if (phase_ == Phase::fence || phase_ == Phase::pre_fence) {
        if ((result.outcome != AYNI_FENCE_DRAINED &&
             result.outcome != AYNI_FENCE_EPOCH_INSTALLED) ||
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
        if (legacy_ && !activation_seen_) {
            phase_ = Phase::migration;
        } else {
            phase_ = Phase::writer_record;
            prepare_append();
        }
        return;
    }
    if (phase_ == Phase::writer_record || phase_ == Phase::boot_record) {
        if (result.outcome != AYNI_WRITE_OK) {
            if (!retry_.failed(host_.monotonic_now_us(host_.ctx))) {
                fail(result.outcome == AYNI_WRITE_FAILED ? AYNI_RESTRICTION_STORAGE_WRITE_FAILED
                                                         : AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
            }
            return;
        }
        const bool is_boot = phase_ == Phase::boot_record;
        const durability::BatchIdentity stored{isolated_epoch_, scan_.next_commit_seq(),
                                               scan_.max_transition_id() + 1U};
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
        return;
    }
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
            if (!activation_matches(activation_, selected_)) {
                fail(AYNI_RESTRICTION_LOG_CORRUPT);
            } else {
                begin_replay();
            }
        }
        break;
    case Phase::discover:
    case Phase::replay:
        static_cast<void>(read_log(result));
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
    case Phase::payload: {
        if (result.value == 0U || result.value > buffer_size_ - carry_ ||
            result.value > selected_.payload_length - payload_read_) {
            fail(AYNI_RESTRICTION_LOG_CORRUPT);
            break;
        }
        payload_crc_ = durability::snapshot_crc32(
            buffer_ + carry_, static_cast<uint32_t>(result.value), payload_crc_);
        payload_read_ += result.value;
        const uint32_t size = carry_ + static_cast<uint32_t>(result.value);
        uint32_t at = 0;
        while (size - at >= durability::record_header_size) {
            const uint8_t *p = buffer_ + at;
            const uint32_t length = foundation::load_le32(p + 8);
            if (length > buffer_size_ - durability::record_header_size) {
                fail(AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
                return;
            }
            if (length > size - at - durability::record_header_size) {
                break;
            }
            const durability::RecordHeader header{foundation::load_le16(p),
                                                  foundation::load_le16(p + 2),
                                                  foundation::load_le32(p + 4), length};
            check(reader_.apply(header, p + durability::record_header_size));
            if (phase_ == Phase::restricted) {
                return;
            }
            at += static_cast<uint32_t>(durability::record_header_size) + length;
        }
        carry_ = size - at;
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
        break;
    }
    default:
        break;
    }
}

bool BootSequence::read_log(const ayni_completion_result_v1 &result) {
    if (result.value == 0U) {
        if (phase_ == Phase::discover) {
            legacy_ = scan_.saw_legacy();
            if (activation_seen_) {
                phase_ = Phase::header;
            } else {
                begin_replay();
            }
        } else {
            after_replay();
        }
        return true;
    }
    if (result.next_cursor <= cursor_) {
        fail(AYNI_RESTRICTION_STORAGE_READ_FAILED);
        return false;
    }
    if (phase_ == Phase::discover) {
        log_bytes_ += result.value;
    }
    cursor_ = result.next_cursor;
    return scan_bytes(static_cast<uint32_t>(result.value));
}

bool BootSequence::scan_bytes(uint32_t size) {
    uint32_t at = 0;
    while (at < size) {
        durability::BatchView batch{};
        const durability::BatchCheck parsed =
            durability::read_batch(buffer_ + at, size - at, batch);
        if (parsed != durability::BatchCheck::ok) {
            check(parsed);
            return false;
        }
        if (phase_ == Phase::discover && first_physical_) {
            first_physical_ = false;
            if (batch.identity.commit_seq != 1U) {
                durability::RecordReader records(batch);
                durability::RecordHeader header{};
                const uint8_t *body = nullptr;
                SnapshotActivation initial{};
                if (records.next(header, body) != durability::RecordReader::Next::record ||
                    batch.record_count != 1U ||
                    header.type != durability::snapshot_commit_record_type ||
                    !read_activation(body, header.length, initial) ||
                    batch.identity.commit_seq != initial.covers_commit_seq + 1U) {
                    fail(AYNI_RESTRICTION_LOG_CORRUPT);
                    return false;
                }
                // A retained older activation may name a slot already reused
                // by a later candidate. Only the latest activation selects a
                // slot; this first record supplies the logical scan boundary.
                scan_.reset(initial.covers_commit_seq + 1U, initial.max_transition_id,
                            batch.identity.writer_epoch);
            }
        }
        if (phase_ != Phase::replay || batch.identity.commit_seq > selected_.covers_commit_seq) {
            check(scan_.scan(buffer_ + at, batch.encoded_size, &BootSequence::on_record, this));
            if (phase_ == Phase::restricted) {
                return false;
            }
            known_durable_seq_ = storage_max(known_durable_seq_, scan_.next_commit_seq() - 1U);
        }
        at += static_cast<uint32_t>(batch.encoded_size);
    }
    return true;
}

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

void BootSequence::after_replay() {
    if (!replay_.complete() || !replay_.consistent()) {
        fail(AYNI_RESTRICTION_LOG_CORRUPT);
    } else if (last_boot_epoch_ >= foundation::max_boot_epoch) {
        fail(AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED);
    } else if (scan_.max_transition_id() > max_logged_transition_id) {
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

void BootSequence::prepare_append() {
    std::array<uint8_t, 8> body{};
    const bool is_boot = phase_ == Phase::boot_record;
    if (is_boot) {
        foundation::store_le64(body.data(), last_boot_epoch_ + 1U);
    } else {
        foundation::store_le32(body.data(), isolated_epoch_);
    }
    durability::BatchWriter writer(
        buffer_, buffer_size_,
        {isolated_epoch_, scan_.next_commit_seq(), scan_.max_transition_id() + 1U});
    const uint16_t type = is_boot ? boot_epoch_record_type : durability::writer_fence_record_type;
    const uint32_t length = is_boot ? 8U : 4U;
    if (!writer.add_record({type, 1, durability::record_required, length}, body.data())) {
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

durability::RecordUse BootSequence::record(const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body) {
    if (header.type == boot_epoch_record_type) {
        if (header.version != 1U) {
            return durability::RecordUse::unknown;
        }
        if (header.length != 8U || foundation::load_le64(body) <= last_boot_epoch_) {
            return durability::RecordUse::corrupt;
        }
        last_boot_epoch_ = foundation::load_le64(body);
        return durability::RecordUse::applied;
    }
    if (header.type == durability::snapshot_commit_record_type) {
        SnapshotActivation candidate{};
        if (header.version != 1U) {
            return durability::RecordUse::unknown;
        }
        if (!read_activation(body, header.length, candidate) ||
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
        return header.type >= 2U && header.type <= 9U && header.version == 1U
                   ? durability::RecordUse::applied
                   : durability::RecordUse::unknown;
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
