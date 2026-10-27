#include "durability/batch/record_body.h"
#include "foundation/little_endian.h"
#include "kernel/service/persistence/persistent_store.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
namespace ayni::kernel {
namespace {
constexpr uint32_t id_max = catalog::CatalogId::max_size;
constexpr uint32_t stamp_size = snapshot_stamp_size;
constexpr uint32_t goal_current_seq_at = work::goal_state_record_size + stamp_size;
constexpr uint32_t goal_extra = goal_current_seq_at + sizeof(uint64_t);

durability::RecordHeader header(uint16_t type, uint16_t version, uint32_t length) {
    return {type, version, durability::record_required, length};
}
// Owns this family's encoding in the original schema 2 journal order.
class Journal {
  public:
    Journal(const WorkModel &model, foundation::TransitionStamp stamp,
            durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer),
          command_(model.commands.mapped_in(stamp.commit_seq)) {}
    bool encode() { return goals() && goal_authority(); }

  private:
    uint8_t *body(uint16_t type, uint16_t version, uint32_t length) {
        uint8_t *at = writer_.add_record_body(header(type, version, length));
        ok_ = ok_ && at != nullptr;
        return at;
    }
    // Creation facts precede all changed states, including a newly created Goal.
    // Version selection protects authority, cleanup and content-source partners.
    bool goals() {
        for (uint32_t index = 0; index < model_.goals.size(); ++index) {
            const work::Goal &goal = model_.goals.at(index);
            if (goal.commit_seq != stamp_.commit_seq ||
                goal.created.transition_id != stamp_.transition_id) {
                continue;
            }
            const uint16_t version = work::goal_record_version_for(
                goal, model_.goal_authority.contains(index, goal.goal_id),
                model_.provenance.find(work::ProvenanceKind::goal, index, goal.goal_id) != nullptr);
            uint8_t *at =
                body(work::goal_record_type, version, work::goal_record_size(goal, version));
            if (at != nullptr) {
                work::write_goal_record(goal, model_.goals.parameters(index), at, version,
                                        model_.provenance.find(work::ProvenanceKind::goal, index,
                                                               goal.goal_id) != nullptr);
            }
        }
        for (uint32_t index = 0; index < model_.goals.size(); ++index) {
            const work::Goal &goal = model_.goals.at(index);
            if (goal.commit_seq != stamp_.commit_seq) {
                continue;
            }
            const bool authority_pause =
                model_.goal_authority.contains(index, goal.goal_id) &&
                model_.goal_authority.at(index).stamp.commit_seq == stamp_.commit_seq &&
                model_.goal_authority.at(index).phase == work::GoalAuthorityPhase::paused;
            const uint16_t version = goal_reauthorized_by(command_, goal.goal_id)
                                         ? work::goal_state_reauthorization_record_version
                                     : authority_pause ? work::goal_state_authority_record_version
                                     : !work::GoalTable::is_live(goal.lifecycle) &&
                                             model_.grants.has_active_goal_grants(goal.goal_id)
                                         ? work::goal_state_with_cleanup_record_version
                                     : model_.goal_authority.contains(index, goal.goal_id)
                                         ? work::goal_state_with_authority_record_version
                                         : work::goal_state_record_version;
            uint8_t *at = body(work::goal_state_record_type, version, work::goal_state_record_size);
            if (at != nullptr) {
                work::write_goal_state_record(goal, at);
            }
        }
        return ok_;
    }
    // Metadata follows Goal state and shares the authority change stamp.
    // Never consult a live catalog while serializing retained durable facts.
    bool goal_authority() {
        uint32_t written = 0;
        for (uint32_t index = 0; index < model_.goals.size(); ++index) {
            work::GoalAuthorityRecord record{};
            if (!model_.goal_authority.read(index, model_.goals.at(index).goal_id, record) ||
                record.stamp.commit_seq != stamp_.commit_seq) {
                continue;
            }
            if (++written > work::goal_authority_records_per_transition ||
                record.stamp.transition_id != stamp_.transition_id) {
                return false;
            }
            const auto version = goal_reauthorized_by(command_, record.goal_id)
                                     ? work::goal_authority_reauthorization_record_version
                                 : record.phase == work::GoalAuthorityPhase::paused
                                     ? work::goal_authority_pause_record_version
                                     : work::goal_authority_record_version;
            auto *at = body(work::goal_authority_record_type, version,
                            work::goal_authority_record_size(record));
            if (at != nullptr) {
                work::write_goal_authority_record(record, at);
            }
        }
        return ok_;
    }
    const WorkModel &model_;
    foundation::TransitionStamp stamp_;
    durability::BatchWriter &writer_;
    const CommandMapping *command_; // Immutable exact mapping for this synchronous encoding.
    bool ok_ = true;
};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    return Journal(model, stamp, writer).encode();
}
// Journal and snapshot versions preserve distinct creation-stamp guarantees.
// Context-specific versions remain unreadable in the other context.
bool knows_goal_body(uint16_t version, WorkRecordContext context) {
    return version == work::goal_record_version ||
           version == work::goal_with_authority_record_version ||
           version == work::goal_authority_state_record_version ||
           (context == WorkRecordContext::journal &&
            (version == work::goal_work_profile_record_version ||
             version == work::goal_provenance_record_version ||
             version == work::goal_calendar_record_version ||
             version == work::goal_calendar_provenance_record_version ||
             version == work::goal_group_record_version)) ||
           (context == WorkRecordContext::snapshot &&
            (version == work::goal_creation_stamp_snapshot_version ||
             version == work::goal_work_profile_snapshot_version ||
             version == work::goal_provenance_snapshot_version ||
             version == work::goal_calendar_snapshot_version ||
             version == work::goal_calendar_provenance_snapshot_version ||
             version == work::goal_group_snapshot_version));
}
bool knows(uint16_t type, uint16_t version, WorkRecordContext context) {
    if (type == work::goal_record_type) {
        return knows_goal_body(version, context);
    }
    if (type == work::goal_authority_record_type) {
        return version == work::goal_authority_record_version ||
               (context == WorkRecordContext::journal &&
                (version == work::goal_authority_pause_record_version ||
                 version == work::goal_authority_reauthorization_record_version));
    }
    // Snapshots carry state inside GOAL; optional journal-only extensions stay skippable.
    if (context == WorkRecordContext::snapshot) {
        return false;
    }
    return version == work::goal_state_record_version ||
           version == work::goal_state_with_authority_record_version ||
           version == work::goal_state_with_cleanup_record_version ||
           (context == WorkRecordContext::journal &&
            (version == work::goal_state_authority_record_version ||
             version == work::goal_state_reauthorization_record_version));
}
uint32_t capacity(const KernelCapacities &c) {
    return c.goal_capacity;
}
// A page holds the larger of creation-with-state and its separate authority row.
uint32_t record_max(const KernelCapacities &c) {
    const auto goal =
        work::goal_record_max_fixed_size + 2U * id_max + c.goal_parameter_max_bytes + goal_extra;
    const auto authority =
        work::goal_authority_record_max(c.goal_requirement_capacity, c.authority_scope_max_bytes);
    return goal > authority ? goal : authority;
}
// Every retained Goal contributes its own row and optional authority partner.
// Use complete per-object bounds rather than multiplying the largest single record.
uint64_t payload_max(const KernelCapacities &c) {
    return uint64_t{c.goal_capacity} *
           (record_bytes(work::goal_record_max_fixed_size + 2U * id_max +
                         c.goal_parameter_max_bytes + goal_extra) +
            record_bytes(work::goal_authority_record_max(c.goal_requirement_capacity,
                                                         c.authority_scope_max_bytes)));
}
// A transition creates at most one Goal but may change every retained state.
// Authority metadata retains its existing one-Goal-per-transition bound.
uint32_t journal_max(const KernelCapacities &c) {
    return record_bytes(work::goal_record_max_fixed_size + 2U * id_max +
                        c.goal_parameter_max_bytes) +
           c.goal_capacity * record_bytes(work::goal_state_record_size) +
           work::goal_authority_records_per_transition *
               record_bytes(work::goal_authority_record_max(c.goal_requirement_capacity,
                                                            c.authority_scope_max_bytes));
}
uint16_t goal_snapshot_version(const WorkModel &model, uint32_t index) {
    const auto &goal = model.goals.at(index);
    return work::goal_snapshot_version_for(
        goal, model.goal_authority.contains(index, goal.goal_id),
        model.provenance.find(work::ProvenanceKind::goal, index, goal.goal_id) != nullptr);
}
// Parent-first traversal survives terminal slot reuse; selection is idempotent.
bool next(const WorkModel &model, StoreCursor &cursor) {
    return next_snapshot_at_depth(model.goals, AYNI_GOAL_DEPTH_MAX_MAX, cursor.order, cursor.index);
}
// The authority part is a distinct envelope immediately after its Goal.
StoreRecord shape(const WorkModel &model, const StoreCursor &cursor) {
    if (cursor.part == StorePart::authority) {
        return {work::goal_authority_record_version,
                work::goal_authority_record_size(model.goal_authority.at(cursor.index)),
                work::goal_authority_record_type};
    }
    const auto version = goal_snapshot_version(model, cursor.index);
    return {version, work::goal_record_size(model.goals.at(cursor.index), version) + goal_extra};
}
// GOAL snapshots retain original creation stamp and current state sequence.
// Authority changes cannot replace the evidence of durable original acceptance.
void write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    if (cursor.part == StorePart::authority) {
        work::write_goal_authority_record(model.goal_authority.at(index), body);
        return;
    }
    const auto &goal = model.goals.at(index);
    const auto version = goal_snapshot_version(model, index);
    const auto base = work::goal_record_size(goal, version);
    work::write_goal_record(
        goal, model.goals.parameters(index), body, version,
        model.provenance.find(work::ProvenanceKind::goal, index, goal.goal_id) != nullptr);
    work::write_goal_state_record(goal, body + base);
    write_snapshot_stamp(body + base + work::goal_state_record_size,
                         {goal.created.transition_id, goal.created.commit_seq});
    foundation::store_le64(body + base + goal_current_seq_at, goal.commit_seq);
}
// Consume only after the complete selected record fits the current page.
void advance(const WorkModel &model, StoreCursor &cursor) {
    if (cursor.part == StorePart::row &&
        model.goal_authority.contains(cursor.index, model.goals.at(cursor.index).goal_id)) {
        cursor.part = StorePart::authority;
    } else {
        ++cursor.index;
        cursor.part = StorePart::row;
    }
}
// Restore creation plus state without inferring a missing historical stamp.
// New authority metadata remains an adjacent required partner across page boundaries.
durability::BatchCheck goal(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            StoreReadCursor &cursor, uint16_t version, const uint8_t *body,
                            uint32_t size) {
    if (version != work::goal_creation_stamp_snapshot_version &&
        version != work::goal_work_profile_snapshot_version &&
        version != work::goal_provenance_snapshot_version &&
        version != work::goal_calendar_snapshot_version &&
        version != work::goal_calendar_provenance_snapshot_version &&
        version != work::goal_group_snapshot_version) {
        return durability::BatchCheck::unsupported; // No inferred creation sequence for old
                                                    // snapshots.
    }
    if (size < work::goal_record_fixed_size + goal_extra) {
        return durability::BatchCheck::corrupt;
    }
    const uint32_t base_size = size - goal_extra;
    work::GoalRecord record{};
    work::GoalStateRecord state{};
    if (!work::read_goal_record(body, base_size, record, version) ||
        !work::read_goal_state_record(body + base_size, work::goal_state_record_size, state) ||
        record.goal_id != state.goal_id ||
        model.agents.find_instance(record.goal.owner_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_snapshot_stamp(body + base_size + work::goal_state_record_size);
    const auto commit_seq = foundation::load_le64(body + base_size + goal_current_seq_at);
    if (!within_snapshot(stamp, metadata) || commit_seq < stamp.commit_seq ||
        commit_seq > metadata.covers_commit_seq) {
        return durability::BatchCheck::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    const auto result = snapshot_restore_check(
        model.goals.restore_snapshot(record.goal, record.goal_id, record.replaced_goal_id,
                                     record.depth, state.state, stamp, commit_seq, index));
    if (result == durability::BatchCheck::ok && record.authority_required) {
        cursor.paired_id = record.goal_id;
        cursor.expected_type = work::goal_authority_record_type;
    }
    if (result == durability::BatchCheck::ok && record.provenance_required &&
        !model.provenance.expect(work::ProvenanceKind::goal, index, record.goal_id)) {
        return durability::BatchCheck::corrupt;
    }
    return result;
}
// Independent metadata may precede ordinary progress, but never creation.
// Exactly one matching authority row closes the selected Goal.
durability::BatchCheck goal_authority(WorkModel &model,
                                      const durability::SnapshotMetadata &metadata,
                                      StoreReadCursor &cursor, const uint8_t *body, uint32_t size) {
    work::GoalAuthorityRecord record{};
    uint32_t index = 0;
    if (!cursor.paired_id || !work::read_goal_authority_record(body, size, record) ||
        record.goal_id != cursor.paired_id || !model.goals.find(record.goal_id, index) ||
        !within_snapshot(record.stamp, metadata) ||
        record.stamp.commit_seq > model.goals.at(index).commit_seq ||
        record.stamp.transition_id < model.goals.at(index).created.transition_id) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model.goal_authority.restore_validated(index, record);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                                : durability::BatchCheck::corrupt;
    }
    cursor.paired_id = 0;
    cursor.expected_type = 0;
    return durability::BatchCheck::ok;
}
durability::BatchCheck read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            uint16_t version, const uint8_t *body, uint32_t size, uint16_t type,
                            StoreReadCursor &cursor) {
    if (type == work::goal_authority_record_type) {
        return goal_authority(model, metadata, cursor, body, size);
    }
    return type == work::goal_record_type ? goal(model, metadata, cursor, version, body, size)
                                          : durability::BatchCheck::unsupported;
}
const StoreKind kinds[] = {
    {work::goal_record_type, work::goal_with_authority_record_version},
    {work::goal_state_record_type, work::goal_state_with_authority_record_version},
    {work::goal_authority_record_type, always_required}};

} // namespace
const PersistentStore goal_store{work::goal_record_type,
                                 kinds,
                                 static_cast<uint32_t>(sizeof(kinds) / sizeof(kinds[0])),
                                 knows,
                                 StorePosition::goals,
                                 capacity,
                                 record_max,
                                 journal_max,
                                 journal,
                                 next,
                                 shape,
                                 write,
                                 advance,
                                 read,
                                 payload_max};
} // namespace ayni::kernel
