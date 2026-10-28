// White-box injection only (C-COMMIT-002/005, E2 F0/F2/F8/F11): locate a
// boundary with the existing batch/record owners, never duplicate their layout.
// Compiled only with AYNI_BUILD_TESTS; no internal type escapes this file.
#include "authority/grant/grant_records.h"
#include "durability/batch/batch_codec.h"
#include "experiment_support.h"
#include "work/call/call_records.h"
namespace ayni::runtime::experiments {
namespace {
std::optional<uint32_t> boundary(const hosts::sim::StoredBatch &stored, RecordPoint point,
                                 const char *step) {
    durability::BatchView batch{};
    if (durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch) !=
        durability::BatchCheck::ok) {
        return std::nullopt;
    }
    if (point == RecordPoint::before_marker) {
        return static_cast<uint32_t>(batch.encoded_size - durability::commit_marker_size);
    }
    durability::RecordReader reader(batch);
    durability::RecordHeader header{};
    const uint8_t *body = nullptr;
    while (reader.next(header, body) == durability::RecordReader::Next::record) {
        bool matched = false;
        switch (point) {
        case RecordPoint::revoked_grant: {
            authority::GrantRevocationRecord grant{};
            matched = header.type == authority::grant_revocation_record_type &&
                      authority::read_grant_revocation_record(body, header.length, grant) &&
                      grant.effect == AYNI_INTENT_EFFECT_CHANGED;
            break;
        }
        case RecordPoint::goal_state:
            matched = header.type == durability::record_type::goal_state;
            break;
        case RecordPoint::task:
            matched = header.type == durability::record_type::task;
            break;
        case RecordPoint::task_state:
            matched = header.type == durability::record_type::task_state;
            break;
        case RecordPoint::task_authority:
            matched = header.type == durability::record_type::task_authority;
            break;
        case RecordPoint::memory:
            matched = header.type == durability::record_type::memory;
            break;
        case RecordPoint::task_checkpoint:
            matched = header.type == durability::record_type::task_checkpoint;
            break;
        case RecordPoint::prepared_call:
        case RecordPoint::dispatch_call:
        case RecordPoint::terminal_call:
        case RecordPoint::stopped_subscription: {
            if (header.type != work::call_record_type) {
                break;
            }
            work::CallRecord call{};
            if (!work::read_call_record(body, header.length, call, header.version) ||
                !call.action) {
                break;
            }
            if (step && !call.identity.step_key.equals(text(step))) {
                break;
            }
            matched = point == RecordPoint::prepared_call
                          ? call.action->phase == work::ActionPhase::prepared
                      : point == RecordPoint::dispatch_call
                          ? call.action->phase == work::ActionPhase::dispatching
                      : point == RecordPoint::stopped_subscription
                          ? call.state.subscription &&
                                call.state.subscription->phase == work::SubscriptionPhase::stopped
                          : call.state.outcome != 0;
            break;
        }
        case RecordPoint::before_marker:
            break;
        }
        if (matched) {
            const auto offset =
                static_cast<std::size_t>(body - stored.bytes.data()) + header.length;
            return offset <= UINT32_MAX ? std::optional<uint32_t>(static_cast<uint32_t>(offset))
                                        : std::nullopt;
        }
    }
    return std::nullopt;
}
} // namespace
bool World::hold_record_append(RecordPoint point, const char *step) {
    items_per_step = 1;
    storage = Storage::complete;
    return until("record append bound", [&] {
        const auto pending = node.simulation()->pending_append();
        if (!pending || !boundary(*pending, point, step)) {
            return false;
        }
        storage = Storage::held;
        evidence.fact("fault", "held_commit_seq", pending->commit_seq);
        evidence.fact("fault", "held_batch_bytes", pending->bytes.size());
        evidence.fact("fault", "record_point", static_cast<uint64_t>(point));
        return true;
    });
}
bool World::crash_after_record(RecordPoint point, const char *step) {
    const auto pending = node.simulation()->pending_append();
    const auto offset = pending ? boundary(*pending, point, step) : std::nullopt;
    if (!pending || !offset) {
        return evidence.status("cut record must be pending", AYNI_NOT_FOUND);
    }
    evidence.fact("fault", "prefix_bytes", *offset);
    evidence.fact("fault", "cut_commit_seq", pending->commit_seq);
    detach_clients();
    return evidence.status("cut exact record prefix", node.power_cut_prefix(*offset));
}
bool World::damage_log(Damage damage) {
    auto &log = node.simulation()->log();
    if (log.empty()) {
        return evidence.status("damage needs stored batch", AYNI_NOT_FOUND);
    }
    auto &batch = log.back();
    if (!evidence.equal("damage framing precondition", batch.torn ? 1 : 0,
                        damage == Damage::torn_tail ? 1 : 0) ||
        !evidence.equal("damage has header", batch.bytes.size() >= durability::batch_header_size,
                        1)) {
        return false;
    }
    // Corrupt the header checksum in actual bytes. A torn Host frame remains a
    // torn tail; a fully framed committed batch must instead restrict recovery.
    constexpr auto checksum_last_byte = durability::batch_header_size - 1;
    batch.bytes[checksum_last_byte] ^= 1;
    durability::BatchView decoded{};
    evidence.fact("fault", "damaged_commit_seq", batch.commit_seq);
    return evidence.equal("damaged batch checksum rejected",
                          durability::read_batch(batch.bytes.data(), batch.bytes.size(), decoded) ==
                              durability::BatchCheck::corrupt,
                          1);
}
} // namespace ayni::runtime::experiments
