#include "kernel/service/persistence/work_replay.h"

#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "authority/scene/scene_records.h"
#include "foundation/kernel_id.h"
#include "kernel/service/command/command_records.h"
#include "knowledge/context/context_records.h"
#include "knowledge/memory/memory_records.h"
#include "work/agent/agent_records.h"
#include "work/authority/authority_cleanup_records.h"
#include "work/call/call_records.h"
#include "work/goal/calendar_records.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
#include "work/privacy/privacy_records.h"
#include "work/provenance/provenance_records.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

using durability::RecordUse;

// Known authority families require a refusal marker even for unknown versions;
// discovery must not skip their ownership semantics as optional metadata.
bool requires_authority_header(const durability::RecordHeader &header) {
    return header.type == work::authority_cleanup_record_type ||
           (header.type == work::task_state_record_type &&
            header.version >= work::task_state_authority_record_version) ||
           header.type == work::task_authority_record_type ||
           header.type == work::goal_authority_record_type ||
           (header.type == work::goal_record_type &&
            header.version >= work::goal_with_authority_record_version) ||
           (header.type == work::goal_state_record_type &&
            header.version >= work::goal_state_with_authority_record_version) ||
           (header.type == work::task_record_type &&
            header.version >= work::task_provenance_snapshot_version) ||
           (header.type == work::task_checkpoint_record_type &&
            header.version >= work::task_checkpoint_provenance_version) ||
           header.type == authority::owner_binding_record_type;
}

bool knows_goal_body(uint16_t version, WorkRecordContext context) {
    return version == work::goal_record_version ||
           version == work::goal_with_authority_record_version ||
           version == work::goal_authority_state_record_version ||
           (context == WorkRecordContext::journal &&
            (version == work::goal_work_profile_record_version ||
             version == work::goal_provenance_record_version ||
             version == work::goal_calendar_record_version ||
             version == work::goal_calendar_provenance_record_version)) ||
           (context == WorkRecordContext::snapshot &&
            (version == work::goal_creation_stamp_snapshot_version ||
             version == work::goal_work_profile_snapshot_version ||
             version == work::goal_provenance_snapshot_version ||
             version == work::goal_calendar_snapshot_version ||
             version == work::goal_calendar_provenance_snapshot_version));
}

template <typename Restore> RecordUse use_of(Restore restore) {
    switch (restore) {
    case Restore::restored:
        return RecordUse::applied;
    case Restore::no_room:
        return RecordUse::no_room;
    case Restore::corrupt:
        break;
    }
    return RecordUse::corrupt;
}

// Ordinary snapshots retain only state; journal-only versions prove their
// authority/reauthorization or owner-command partners (ADR-0043).
bool knows_task_state(uint16_t version, WorkRecordContext context) {
    return version == work::task_state_record_version ||
           version == work::task_state_runtime_record_version ||
           version == work::task_state_call_record_version ||
           (context == WorkRecordContext::journal &&
            (version == work::task_state_authority_record_version ||
             version == work::task_state_reauthorization_record_version ||
             version == work::task_state_runtime_authority_record_version ||
             version == work::task_state_runtime_reauthorization_record_version ||
             version == work::task_state_raise_limit_record_version ||
             version == work::task_state_call_authority_record_version ||
             version == work::task_state_call_reauthorization_record_version ||
             version == work::task_state_call_raise_limit_record_version ||
             version == work::task_state_resume_wait_record_version));
}

} // namespace

RecordUse WorkReplay::on_record(void *context, const durability::BatchIdentity &batch,
                                const durability::RecordHeader &header, const uint8_t *body,
                                uint64_t boot_epoch_limit) {
    return static_cast<WorkReplay *>(context)->apply(batch, header, body, boot_epoch_limit);
}

bool WorkReplay::binding_matches() const {
    if (model_.principals.binding_known()) {
        return model_.principals.binding_matches();
    }
    // Schema 2 never adopts existing owner-controlled facts from an unbound
    // store. Empty and agent-only stores can still bind their first owner;
    // missing evidence is refused before the boot writes (ADR-0038).
    return model_.goals.size() == 0U && model_.commands.size() == 0U &&
           model_.grants.size() == 0U && model_.scenes.state_count() == 0 &&
           model_.memories.size() == 0;
}

// The types apply() restores, so the two lists change together; a store of
// persistent_store.h states its own versions.
// Discovery must enforce ownership's header rule even when a snapshot
// covers the record and its body will not be replayed.
RecordUse WorkReplay::classify_header(const durability::RecordHeader &header,
                                      WorkRecordContext context) {
    uint32_t rank = 0;
    const bool store = find_persistent_store(header.type, rank);
    if (((store && store_requires_marker(*persistent_stores[rank], header.type, header.version)) ||
         requires_authority_header(header)) &&
        (header.flags & durability::record_required) == 0U) {
        return RecordUse::corrupt;
    }
    if (store) {
        return persistent_stores[rank]->knows(header.type, header.version, context)
                   ? RecordUse::applied
                   : RecordUse::unknown;
    }
    switch (header.type) {
    case work::goal_record_type:
        return knows_goal_body(header.version, context) ? RecordUse::applied : RecordUse::unknown;
    case work::goal_state_record_type:
        return header.version == work::goal_state_record_version ||
                       header.version == work::goal_state_with_authority_record_version ||
                       header.version == work::goal_state_with_cleanup_record_version ||
                       (context == WorkRecordContext::journal &&
                        (header.version == work::goal_state_authority_record_version ||
                         header.version == work::goal_state_reauthorization_record_version))
                   ? RecordUse::applied
                   : RecordUse::unknown;
    case work::task_authority_record_type:
        return header.version == work::task_authority_record_version ? RecordUse::applied
                                                                     : RecordUse::unknown;
    case work::goal_authority_record_type:
        return header.version == work::goal_authority_record_version ||
                       (context == WorkRecordContext::journal &&
                        (header.version == work::goal_authority_pause_record_version ||
                         header.version == work::goal_authority_reauthorization_record_version))
                   ? RecordUse::applied
                   : RecordUse::unknown;
    case work::authority_cleanup_record_type:
        return header.version == work::authority_cleanup_record_version ? RecordUse::applied
                                                                        : RecordUse::unknown;
    case authority::owner_binding_record_type:
    case work::instance_record_type:
    case work::task_steps_record_type:
        return header.version == 1U ? RecordUse::applied : RecordUse::unknown;
    case work::task_record_type:
        return header.version == work::task_record_version ||
                       header.version == work::task_calendar_record_version ||
                       (context == WorkRecordContext::snapshot &&
                        (header.version == work::task_provenance_snapshot_version ||
                         header.version == work::task_calendar_provenance_snapshot_version))
                   ? RecordUse::applied
                   : RecordUse::unknown;
    case work::task_checkpoint_record_type:
        return header.version == work::task_checkpoint_record_version ||
                       header.version == work::task_checkpoint_provenance_version
                   ? RecordUse::applied
                   : RecordUse::unknown;
    case work::task_state_record_type:
        return knows_task_state(header.version, context) ? RecordUse::applied : RecordUse::unknown;
    default:
        return RecordUse::unknown;
    }
}

RecordUse WorkReplay::apply(const durability::BatchIdentity &batch,
                            const durability::RecordHeader &header, const uint8_t *body,
                            uint64_t boot_epoch_limit) {
    // C-STORE-015/ADR-0031: no unmatched work state, authority command or
    // cleanup progress may cross the batch boundary before this record.
    if (batch.commit_seq != batch_seq_) {
        if (!complete()) {
            return RecordUse::corrupt;
        }
        pairing_.approval.reset();
        pairing_.limit_raise.reset();
        pairing_.target_command.reset();
        pairing_.scene.reset();
        pairing_.memory.reset();
        pairing_.context.reset();
        pairing_.privacy_progress = false;
        pairing_.reauthorization.reset();
        pairing_.work_cleanup = {};
    }
    // A completed approval seals this batch (C-STORE-019); preserve its proof
    // until the next batch so unrelated trailing records cannot be accepted.
    if (pairing_.privacy_progress || (pairing_.memory && pairing_.memory->complete()) ||
        (pairing_.context && pairing_.context->complete()) ||
        (pairing_.scene && pairing_.scene->command_seen) ||
        (pairing_.target_command && pairing_.target_command->command_seen) ||
        (pairing_.limit_raise &&
         pairing_.limit_raise->stage == BatchPairing::TaskLimitPair::Stage::command) ||
        pairing_.work_cleanup.stage == BatchPairing::WorkCleanupPair::Stage::complete ||
        (pairing_.approval && pairing_.approval->stage == ApprovalPair::Stage::complete)) {
        return RecordUse::corrupt;
    }
    const auto use = apply_record(batch, header, body, boot_epoch_limit);
    batch_seq_ = batch.commit_seq;
    return use;
}

// Preserve the exact atomic partners while dispatch remains a short list of invariant owners.
bool WorkReplay::allows_pending_record(const durability::RecordHeader &header) const {
    if (pairing_.context) {
        const auto &pair = *pairing_.context;
        const auto expected = pair.expected.kind == AYNI_INTENT_CONTEXT_FORGET && !pair.privacy_seen
                                  ? work::privacy_record_type
                                  : command_record_type;
        if (header.type != expected) {
            return false;
        }
    }
    if (pairing_.memory) {
        const auto &pair = *pairing_.memory;
        const auto expected =
            pair.policy_task_id != 0 && !pair.command_seen ? work::provenance_record_type
            : pair.privacy_required && !pair.privacy_seen  ? work::privacy_record_type
                                                           : command_record_type;
        if (header.type != expected) {
            return false;
        }
    }
    if ((pairing_.scene || pairing_.limit_raise || pairing_.target_command) &&
        header.type != command_record_type) {
        return false;
    }
    // A pending approval admits only its own Grants/state/metadata/command.
    // Unrelated work cannot smuggle an additional transition into the batch.
    if (pairing_.approval && header.type != authority::grant_record_type &&
        header.type != work::goal_state_record_type &&
        header.type != work::goal_authority_record_type && header.type != command_record_type) {
        return false;
    }
    if (!allows_reauthorization_record(header)) {
        return false;
    }
    // The new Task's state closes directly with its admission (C-STORE-020).
    // A different record cannot overwrite or hide this pending evidence.
    if (pairing_.task_authority && !pairing_.task &&
        header.type != work::task_authority_record_type) {
        return false;
    }
    // A metadata-only Goal state also closes immediately; another state
    // cannot overwrite its exact pending authority partner (C-STORE-021).
    if (pairing_.work_cleanup.state_goal_id != 0U && pairing_.goal_authority &&
        header.type != work::goal_authority_record_type) {
        return false;
    }
    return true;
}

RecordUse WorkReplay::apply_record(const durability::BatchIdentity &batch,
                                   const durability::RecordHeader &header, const uint8_t *body,
                                   uint64_t boot_epoch_limit) {
    if (!allows_pending_record(header)) {
        return RecordUse::corrupt;
    }
    const foundation::TransitionStamp stamp{batch.transition_id, batch.commit_seq};
    const uint32_t size = header.length;
    const RecordUse use = classify_header(header);
    if (use != RecordUse::applied) {
        return use;
    }
    switch (header.type) {
    case work::provenance_record_type:
        return restore_provenance(header.version, body, size, stamp);
    case work::privacy_record_type:
        return restore_privacy(body, size, stamp, boot_epoch_limit);
    case knowledge::context_record_type:
        return restore_context(body, size, stamp, boot_epoch_limit);
    case knowledge::memory_record_type:
        return restore_memory(body, size, stamp, boot_epoch_limit);
    case work::calendar_record_type:
        return restore_calendar(body, size, stamp);
    case authority::scene_record_type:
        return restore_scene(body, size, stamp, boot_epoch_limit);
    case authority::owner_binding_record_type:
        return restore_owner_binding(body, size, stamp);
    case authority::grant_record_type:
        return restore_grant(header.version, body, size, stamp, boot_epoch_limit);
    case authority::grant_state_record_type:
        return restore_grant_state(header.version, body, size, stamp);
    case authority::grant_revocation_record_type:
        return restore_revocation(header.version, body, size, stamp, boot_epoch_limit);
    case authority::grant_revocation_progress_record_type:
        return restore_revocation_progress(body, size, stamp);
    case work::instance_record_type:
        return restore_instance(body, size, stamp);
    case work::goal_record_type:
        return restore_goal(header.version, body, size, stamp);
    case work::goal_authority_record_type:
        return restore_goal_authority(header.version, body, size, stamp, boot_epoch_limit);
    case work::authority_cleanup_record_type:
        return restore_authority_cleanup(body, size, stamp);
    case work::goal_state_record_type:
        return restore_goal_state(header.version, body, size, stamp);
    case work::task_authority_record_type:
        return restore_task_authority(body, size, {batch.transition_id, batch.commit_seq});
    case work::task_record_type:
        return restore_task(header.version, body, size, stamp);
    case work::call_record_type:
        return restore_call(header.version, body, size, {boot_epoch_limit, stamp});
    case work::task_state_record_type:
        return restore_task_state(header.version, body, size, stamp);
    case work::task_steps_record_type:
        return restore_task_steps(body, size, stamp);
    case work::task_checkpoint_record_type:
        return restore_task_checkpoint(header.version, body, size, stamp);
    case command_record_type:
        return restore_command(header.version, body, size, batch.commit_seq, boot_epoch_limit);
    default:
        return RecordUse::unknown;
    }
}

static_assert(authority::owner_binding_record_version == 1U &&
                  work::instance_record_version == 1U && work::goal_record_version == 1U &&
                  work::goal_state_record_version == 1U && work::task_record_version == 1U &&
                  work::goal_with_authority_record_version == 2U &&
                  work::goal_authority_record_version == 2U &&
                  work::task_state_record_version == 1U && work::task_steps_record_version == 1U &&
                  work::task_checkpoint_record_version == 1U && command_record_version == 1U &&
                  authority::grant_record_version == 1U &&
                  authority::grant_state_record_version == 1U &&
                  exact_command_record_version == 2U && revoke_command_record_version == 3U &&
                  authority::grant_revoked_state_record_version == 2U &&
                  authority::grant_snapshot_record_version == 2U &&
                  authority::grant_revocation_record_version == 1U &&
                  authority::grant_revocation_progress_record_version == 1U,
              "classify_header/apply/snapshot know every current work and authority version");

RecordUse WorkReplay::restore_owner_binding(const uint8_t *body, uint32_t size,
                                            foundation::TransitionStamp stamp) {
    authority::OwnerBindingRecord record{};
    if (!authority::read_owner_binding_record(body, size, record) ||
        !model_.principals.restore_binding(record, stamp)) {
        return RecordUse::corrupt;
    }
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_instance(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp) {
    work::InstanceRecord record{};
    if (!work::read_instance_record(body, size, record)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.agents.restore(record.agent_id, record.instance_id, record.revision,
                                        record.lifecycle, stamp));
}

RecordUse WorkReplay::restore_goal(uint16_t version, const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::GoalRecord record{};
    // A goal is created for an instance that has started.
    if (pairing_.goal.has_value() || pairing_.goal_authority.has_value() ||
        !work::read_goal_record(body, size, record, version) ||
        model_.agents.find_instance(record.goal.owner_instance_id) == nullptr) {
        return RecordUse::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    const work::GoalTable::Restore restore =
        model_.goals.restore(record.goal, record.goal_id, record.replaced_goal_id, stamp, index);
    if (restore != work::GoalTable::Restore::restored) {
        return use_of(restore);
    }
    // The goal that gave way took its tasks, which name the place, with it
    // (work::insert_goal).
    if (record.replaced_goal_id != 0U) {
        model_.retire_goal_task_metadata(index, stamp.commit_seq);
        model_.tasks.remove_goal(index);
    }
    if (model_.goals.at(index).depth != record.depth) {
        return RecordUse::corrupt;
    }
    model_.retire_goal_metadata(index, record.replaced_goal_id, stamp.commit_seq);
    pairing_.goal = record.goal_id;
    if (record.authority_required) {
        pairing_.goal_authority = record.goal_id;
    }
    if (record.provenance_required) {
        pairing_.goal_sources = record.goal_id;
    }
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_goal_state(uint16_t version, const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::GoalStateRecord record{};
    uint32_t index = 0;
    if (!work::read_goal_state_record(body, size, record) ||
        !model_.goals.find(record.goal_id, index) ||
        (version >= work::goal_state_with_authority_record_version &&
         pairing_.goal_authority != record.goal_id &&
         !model_.goal_authority.contains(index, record.goal_id)) ||
        (version == work::goal_state_record_version &&
         record.state.lifecycle == AYNI_GOAL_PROPOSED)) {
        return RecordUse::corrupt;
    }
    if (version == work::goal_state_reauthorization_record_version) {
        return begin_reauthorization(index, stamp)
                   ? use_of(model_.goals.restore_reauthorization_state(index, record.state, stamp))
                   : RecordUse::corrupt;
    }
    if (!pair_approval_state(index, record.state)) {
        return RecordUse::corrupt;
    }
    if (version == work::goal_state_authority_record_version) {
        pairing_.work_cleanup.state_goal_id = record.goal_id;
        pairing_.work_cleanup.require();
        pairing_.goal_authority = record.goal_id;
        return use_of(model_.goals.restore_authority_state(index, record.state, stamp));
    }
    if (record.goal_id == pairing_.goal) {
        pairing_.goal.reset();
    }
    if (version == work::goal_state_with_cleanup_record_version) {
        if (work::GoalTable::is_live(record.state.lifecycle)) {
            return RecordUse::corrupt;
        }
        pairing_.work_cleanup.require();
    }
    return use_of(model_.goals.restore_state(index, record.state, stamp));
}

// A create cannot lose its explicit ceiling or borrow another Goal's metadata.
// Existing metadata may advance without changing its immutable ceiling;
// F4 adds a separate paired proof for explicit reauthorization.
RecordUse WorkReplay::restore_goal_authority(uint16_t version, const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp,
                                             uint64_t boot_epoch_limit) {
    work::GoalAuthorityRecord record{};
    uint32_t index = 0;
    if (pairing_.goal_authority_seq == stamp.commit_seq ||
        !work::read_goal_authority_record(body, size, record) ||
        !model_.goals.find(record.goal_id, index) ||
        record.binding.input.incarnation > boot_epoch_limit ||
        record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.transition_id != stamp.transition_id ||
        model_.goals.at(index).commit_seq != stamp.commit_seq || pairing_.goal.has_value()) {
        return RecordUse::corrupt;
    }
    if ((version == work::goal_authority_pause_record_version) !=
            (pairing_.work_cleanup.state_goal_id == record.goal_id) ||
        (version == work::goal_authority_pause_record_version &&
         record.phase != work::GoalAuthorityPhase::paused)) {
        return RecordUse::corrupt;
    }
    if (pairing_.goal_authority ? *pairing_.goal_authority != record.goal_id
                                : !model_.goal_authority.contains(index, record.goal_id)) {
        return RecordUse::corrupt;
    }
    if ((!model_.goal_authority.contains(index, record.goal_id) &&
         record.phase != work::GoalAuthorityPhase::proposed &&
         !matches_goal_binding(index, record, stamp.commit_seq)) ||
        !(version == work::goal_authority_reauthorization_record_version
              ? pair_reauthorization_metadata(index, record, stamp)
              : pair_approval_metadata(index, record))) {
        return RecordUse::corrupt;
    }
    work::GoalAuthorityRecord previous{};
    const bool replacing = model_.goal_authority.read(index, record.goal_id, previous);
    if (version == work::goal_authority_pause_record_version &&
        (!replacing || previous.phase != work::GoalAuthorityPhase::authorized)) {
        return RecordUse::corrupt;
    }
    const auto status = model_.goal_authority.restore_validated(index, record);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? RecordUse::no_room : RecordUse::corrupt;
    }
    if (version == work::goal_authority_reauthorization_record_version &&
        !goal_authority_consistent(index, boot_epoch_limit)) {
        return RecordUse::corrupt;
    }
    if (replacing) {
        model_.release_goal_references(previous, stamp.commit_seq);
    }
    pairing_.goal_authority.reset();
    pairing_.goal_authority_seq = stamp.commit_seq;
    if (version == work::goal_authority_pause_record_version) {
        pairing_.work_cleanup.require();
        pairing_.work_cleanup.paused_goal_id = record.goal_id;
    }
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_task(uint16_t version, const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::TaskRecord record{};
    uint32_t goal_index = 0;
    if (pairing_.task_created_seq == stamp.commit_seq || pairing_.task.has_value() ||
        !work::read_task_record(body, size, record, version) ||
        !model_.goals.find(record.task.goal_id, goal_index) ||
        model_.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return RecordUse::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore =
        model_.tasks.restore(record.task, record.task_id, record.replaced_task_id, stamp);
    if (restore == work::TaskTable::Restore::restored) {
        model_.calls.retire_task(record.replaced_task_id);
        uint32_t task_index = 0;
        if (!model_.tasks.find(record.task_id, task_index)) {
            return RecordUse::corrupt;
        }
        model_.clear_content_provenance(work::ProvenanceKind::task,
                                        model_.tasks.at(task_index).slot, record.replaced_task_id,
                                        stamp.commit_seq);
        pairing_.task_created_seq = stamp.commit_seq;
        pairing_.task = record.task_id;
        pairing_.task_authority = record.task_id;
    }
    return use_of(restore);
}

// A derived Task inherits the exact admitted Goal choices in the same batch.
// F4 adds a separate transaction proof for reference replacement.
RecordUse WorkReplay::restore_task_authority(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp) {
    work::TaskAuthorityRecord record{};
    uint32_t index = 0;
    if (!work::read_task_authority_record(body, size, record) ||
        pairing_.task_authority != record.task_id || pairing_.task ||
        !model_.tasks.find(record.task_id, index) || record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.transition_id != stamp.transition_id ||
        !model_.matches_task_ceiling(record, model_.tasks.at(index).goal_index)) {
        return RecordUse::corrupt;
    }
    const auto &task = model_.tasks.at(index);
    const bool created = task.created_transition_id == stamp.transition_id;
    const auto ceiling = model_.goal_authority.at(task.goal_index);
    const bool reauthorizing = pairing_.reauthorization.has_value();
    if ((created || reauthorizing) ? record.phase != work::TaskAuthorityPhase::authorized
                                   : record.phase != work::TaskAuthorityPhase::paused ||
                                         ceiling.phase != work::GoalAuthorityPhase::paused ||
                                         ceiling.stamp.commit_seq != stamp.commit_seq) {
        return RecordUse::corrupt;
    }
    work::TaskAuthorityRecord previous{};
    if (model_.task_authority.read(task.slot, created ? task.replaced_task_id : task.task_id,
                                   previous)) {
        model_.release_task_references(previous, stamp.commit_seq);
    }
    const auto result = created ? model_.task_authority.install_validated(task.slot, record)
                                : model_.task_authority.restore_validated(task.slot, record);
    if (result != AYNI_OK) {
        return result == AYNI_CAPACITY_EXCEEDED ? RecordUse::no_room : RecordUse::corrupt;
    }
    pairing_.task_authority.reset();
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_task_state(uint16_t version, const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskStateRecord record{};
    uint32_t index = 0;
    if (!work::read_task_state_record(body, size, record, version) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    if (record.task_id == pairing_.task) {
        pairing_.task.reset();
    }
    if (version == work::task_state_resume_wait_record_version) {
        // Standalone wait resume preserves the original call and requires exact owner identity.
        if (batch_seq_ == stamp.commit_seq || !pairing_.complete()) {
            return RecordUse::corrupt;
        }
        pairing_.target_command = BatchPairing::TargetCommandPair{
            {AYNI_INTENT_TASK_RESUME_WAIT, model_.tasks.at(index).revision, record.task_id}};
        return use_of(model_.tasks.restore_wait_resume(index, record.state, stamp));
    }
    if (work::task_state_base_version(version) == work::task_state_raise_limit_record_version) {
        // A raise is a standalone owner command, sealed by its exact identity.
        if (batch_seq_ == stamp.commit_seq || !pairing_.complete()) {
            return RecordUse::corrupt;
        }
        pairing_.limit_raise = BatchPairing::TaskLimitPair{
            {model_.tasks.at(index).revision, record.task_id, record.state.model_calls.limit}};
        return use_of(model_.tasks.restore_model_limit_raise(index, record.state, stamp));
    }
    version = work::task_state_base_version(version);
    if (version == work::task_state_reauthorization_record_version) {
        work::TaskAuthorityRecord admission{};
        if (!pairing_.reauthorization ||
            pairing_.reauthorization->stage != BatchPairing::ReauthorizationPair::Stage::metadata ||
            model_.tasks.at(index).goal_id != pairing_.reauthorization->goal_id ||
            !model_.task_authority.read(model_.tasks.at(index).slot, record.task_id, admission) ||
            admission.ceiling.commit_seq != pairing_.reauthorization->binding_seq) {
            return RecordUse::corrupt;
        }
        pairing_.task_authority = record.task_id;
        return use_of(model_.tasks.restore_reauthorization_state(index, record.state, stamp));
    }
    if (version == work::task_state_authority_record_version) {
        if (model_.tasks.at(index).created_transition_id == stamp.transition_id) {
            return RecordUse::corrupt;
        }
        pairing_.work_cleanup.require();
        pairing_.task_authority = record.task_id;
        return use_of(model_.tasks.restore_authority_state(index, record.state, stamp));
    }
    return use_of(model_.tasks.restore_state(index, record.state, stamp));
}

RecordUse WorkReplay::restore_task_steps(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskTable &tasks = model_.tasks;
    uint64_t task_id = 0;
    uint32_t index = work::TaskTable::no_index;
    work::TaskTable::Restore failure = work::TaskTable::Restore::corrupt;
    const bool read =
        work::read_task_steps_record(body, size, task_id, [&](uint32_t step, ayni_string_v1 key) {
            if (index == work::TaskTable::no_index && !tasks.find(task_id, index)) {
                return false;
            }
            const work::TaskTable::Restore restore = tasks.restore_step(index, step, key, stamp);
            if (restore != work::TaskTable::Restore::restored) {
                failure = restore;
                return false;
            }
            return true;
        });
    return read ? RecordUse::applied : use_of(failure);
}

RecordUse WorkReplay::restore_task_checkpoint(uint16_t version, const uint8_t *body, uint32_t size,
                                              foundation::TransitionStamp stamp) {
    work::TaskCheckpointRecord record{};
    uint32_t index = 0;
    if (!work::read_task_checkpoint_record(body, size, record) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    const auto &task = model_.tasks.at(index);
    const bool tracked = version == work::task_checkpoint_provenance_version;
    if (pairing_.checkpoint_sources ||
        (!tracked &&
         model_.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id) != nullptr)) {
        return RecordUse::corrupt;
    }
    if (tracked) {
        pairing_.checkpoint_sources = task.task_id;
    }
    return use_of(model_.tasks.restore_checkpoint(index, record.writer_id, record.writer_version,
                                                  record.state, stamp));
}

// C-STORE-015: create and its exact command share this batch. Validate the
// already durable issuer/subject before publishing a fact awaiting that command.
RecordUse WorkReplay::restore_grant(uint16_t version, const uint8_t *body, uint32_t size,
                                    foundation::TransitionStamp stamp, uint64_t boot_epoch_limit) {
    authority::GrantRecord record{};
    const auto *binding = model_.principals.binding();
    if (!authority::read_grant_record(body, size, record, version) ||
        foundation::id_boot_epoch(record.grant_id) != boot_epoch_limit) {
        return RecordUse::corrupt;
    }
    if (pairing_.authority.pending() || model_.commands.mapped_in(stamp.commit_seq) != nullptr ||
        binding == nullptr || binding->principal_id != record.grant.owner_principal_id ||
        binding->binding_revision != record.grant.owner_binding_revision ||
        binding->stamp.commit_seq >= stamp.commit_seq ||
        binding->stamp.transition_id >= stamp.transition_id ||
        model_.agents.find_instance(record.grant.subject_instance_id) == nullptr) {
        return RecordUse::corrupt;
    }
    if (record.grant.bound_goal_id != 0U) {
        if (!pair_approval_grant(record, stamp)) {
            return RecordUse::corrupt;
        }
    } else if (pairing_.approval) {
        return RecordUse::corrupt;
    }
    const auto restored = model_.grants.restore_create(
        record, stamp, model_.grant_reclaim_exclusions(stamp.commit_seq - 1U));
    if (restored == authority::GrantTable::Restore::restored && record.grant.bound_goal_id == 0U) {
        pairing_.authority = {AuthorityPairKind::create, record.grant_id};
    }
    return use_of(restored);
}

// Expiry is standalone; descendant revocation must be the only unmatched
// authority fact and precede its same-batch progress record (ADR-0031).
RecordUse WorkReplay::restore_grant_state(uint16_t version, const uint8_t *body, uint32_t size,
                                          foundation::TransitionStamp stamp) {
    authority::GrantStateRecord record{};
    if (!authority::read_grant_state_record(body, size, record, version)) {
        return RecordUse::corrupt;
    }
    if (record.lifecycle == AYNI_GRANT_REVOKED &&
        (pairing_.authority.pending() || pairing_.progress_seq == stamp.commit_seq ||
         model_.commands.mapped_in(stamp.commit_seq) != nullptr)) {
        return RecordUse::corrupt;
    }
    if (record.lifecycle == AYNI_GRANT_GOAL_ENDED) {
        uint32_t grant = 0;
        uint32_t goal = 0;
        if (!model_.grants.find(record.grant_id, grant) ||
            (model_.goals.find(model_.grants.at(grant).bound_goal_id, goal) &&
             (work::GoalTable::is_live(model_.goals.at(goal).lifecycle) ||
              model_.goals.at(goal).commit_seq >= stamp.commit_seq))) {
            return RecordUse::corrupt;
        }
        const auto bound = model_.grants.at(grant).bound_goal_id;
        if (pairing_.work_cleanup.ended_goal_id != 0U &&
            pairing_.work_cleanup.ended_goal_id != bound) {
            return RecordUse::corrupt;
        }
        pairing_.work_cleanup.ended_goal_id = bound;
    }
    const auto restored = model_.grants.restore_state(record, stamp);
    if (restored == authority::GrantTable::Restore::restored &&
        record.lifecycle == AYNI_GRANT_REVOKED) {
        pairing_.authority = {AuthorityPairKind::cleanup_child, record.grant_id,
                              record.revoked_by_root_id};
    }
    if (restored == authority::GrantTable::Restore::restored &&
        (version == authority::grant_goal_ended_state_record_version ||
         version == authority::grant_expired_with_work_record_version)) {
        pairing_.work_cleanup.require();
    }
    return use_of(restored);
}

// C-AUTH-027: replay accepted root control under the saved issuer binding;
// its effect/revision must be matched by the following exact command record.
RecordUse WorkReplay::restore_revocation(uint16_t version, const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp,
                                         uint64_t boot_epoch_limit) {
    authority::GrantRevocationRecord record{};
    const auto *binding = model_.principals.binding();
    if (pairing_.authority.pending() || model_.commands.mapped_in(stamp.commit_seq) != nullptr ||
        binding == nullptr || binding->stamp.commit_seq >= stamp.commit_seq ||
        binding->stamp.transition_id >= stamp.transition_id ||
        !authority::read_grant_revocation_record(body, size, record) ||
        foundation::id_boot_epoch(record.grant_id) == 0U ||
        foundation::id_boot_epoch(record.grant_id) > boot_epoch_limit) {
        return RecordUse::corrupt;
    }
    const auto restored = model_.grants.restore_revocation(record, stamp);
    if (restored == authority::GrantTable::Restore::restored) {
        pairing_.authority = {AuthorityPairKind::revocation, record.grant_id, 0, record.revision,
                              record.effect};
        if (version == authority::grant_revocation_with_work_record_version) {
            pairing_.work_cleanup.require();
        }
    }
    return use_of(restored);
}

// C-AUTH-026: at most one progress per batch, paired with any revoked child.
// Skipping an unrelated candidate needs no child state; GrantTable checks its ID.
RecordUse WorkReplay::restore_revocation_progress(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp) {
    authority::GrantRevocationProgressRecord record{};
    if (pairing_.progress_seq == stamp.commit_seq || pairing_.authority.awaiting_command() ||
        model_.commands.mapped_in(stamp.commit_seq) != nullptr ||
        !authority::read_grant_revocation_progress_record(body, size, record) ||
        (pairing_.authority.kind == AuthorityPairKind::cleanup_child &&
         (pairing_.authority.root_id != record.root_grant_id ||
          pairing_.authority.grant_id != record.after_grant_id))) {
        return RecordUse::corrupt;
    }
    const auto restored = model_.grants.restore_progress(record, stamp);
    if (restored == authority::GrantTable::Restore::restored) {
        pairing_.progress_seq = stamp.commit_seq;
        pairing_.authority = {};
    }
    return use_of(restored);
}

// Close the saved Task ceiling change with its exact owner command.
bool WorkReplay::pair_task_limit_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    if (mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT) {
        TaskLimitCommand command{};
        const auto *binding = model_.principals.binding();
        if (!pairing_.limit_raise || !read_task_limit_command(canonical, command) ||
            !(command == pairing_.limit_raise->request) || binding == nullptr ||
            mapping.principal_id != binding->principal_id ||
            binding->stamp.commit_seq >= mapping.commit_seq) {
            return false;
        }
        pairing_.limit_raise->stage = BatchPairing::TaskLimitPair::Stage::command;
    } else if (pairing_.limit_raise) {
        return false;
    }
    return true;
}

// Each domain owns its exact request proof. No unrelated pending partner may be skipped.
bool WorkReplay::pair_domain_command(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                     ayni_bytes_v1 result_bytes) {
    if (is_context_command(mapping.kind)) {
        if (!pair_context_command(mapping, canonical)) {
            return false;
        }
    } else if (pairing_.context) {
        return false;
    }
    if (is_memory_command(mapping.kind)) {
        if (!pair_memory_command(mapping, canonical)) {
            return false;
        }
    } else if (pairing_.memory) {
        return false;
    }
    if (mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
        if (!pair_scene_command(mapping, canonical)) {
            return false;
        }
    } else if (pairing_.scene) {
        return false;
    }
    if (is_target_command(mapping.kind)) {
        if (!pair_target_command(mapping, canonical)) {
            return false;
        }
    } else if (pairing_.target_command) {
        return false;
    }
    if (!pair_task_limit_command(mapping, canonical)) {
        return false;
    }
    if (mapping.kind == AYNI_INTENT_GOAL_APPROVE) {
        if (!pair_approval_command(mapping, canonical, result_bytes)) {
            return false;
        }
    } else if (pairing_.approval) {
        return false;
    }
    if (mapping.kind == AYNI_INTENT_GOAL_REAUTHORIZE) {
        if (!pair_reauthorization_command(mapping, canonical, result_bytes)) {
            return false;
        }
    } else if (pairing_.reauthorization) {
        return false;
    }
    return true;
}

// An absent principal's retry window is still durable state. Keeping it
// prevents compaction from turning an old accepted command into a new one.
RecordUse WorkReplay::restore_command(uint16_t version, const uint8_t *body, uint32_t size,
                                      uint64_t commit_seq, uint64_t boot_epoch_limit) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    ayni_bytes_v1 result_bytes{};
    if (model_.commands.mapped_in(commit_seq) != nullptr ||
        !read_command_record(version, body, size, mapping, floor, canonical, &result_bytes)) {
        return RecordUse::corrupt;
    }
    mapping.commit_seq = commit_seq;
    if (!pair_domain_command(mapping, canonical, result_bytes) ||
        !matches_command_sources(mapping, boot_epoch_limit)) {
        return RecordUse::corrupt;
    }
    if (mapping.sources.count > model_.memories.limits().dependencies) {
        return RecordUse::no_room;
    }
    uint32_t goal = 0;
    if (mapping.object.kind == AYNI_OBJECT_GOAL && model_.goals.find(mapping.object.id, goal)) {
        const auto *record =
            model_.provenance.find(work::ProvenanceKind::goal, goal, mapping.object.id);
        const knowledge::MemoryDependencies expected =
            record == nullptr ? knowledge::MemoryDependencies{} : record->dependencies;
        if (!knowledge::same_dependencies(mapping.sources, expected)) {
            return RecordUse::corrupt;
        }
    }
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE) {
        uint32_t index = 0;
        if (pairing_.authority.kind != AuthorityPairKind::create ||
            mapping.object.id != pairing_.authority.grant_id ||
            !model_.grants.find(pairing_.authority.grant_id, index)) {
            return RecordUse::corrupt;
        }
        const auto &grant = model_.grants.at(index);
        if (!matches_grant_creation(mapping, canonical, grant, model_.grants.source_domain(index),
                                    model_.grants.scope(index))) {
            return RecordUse::corrupt;
        }
        pairing_.authority = {};
    } else if (mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        RevokeCommandRecord request{};
        const auto *binding = model_.principals.binding();
        if (pairing_.authority.kind != AuthorityPairKind::revocation ||
            mapping.object.id != pairing_.authority.grant_id ||
            mapping.object.revision != pairing_.authority.revision ||
            mapping.effect != pairing_.authority.effect || binding == nullptr ||
            mapping.principal_id != binding->principal_id ||
            binding->stamp.commit_seq >= commit_seq || !read_revoke_command(canonical, request) ||
            request.target_grant_id != mapping.object.id) {
            return RecordUse::corrupt;
        }
        pairing_.authority = {};
    }
    model_.release_command_sources(mapping.principal_id, mapping.commit_seq);
    return use_of(model_.commands.restore_checked(mapping, floor, canonical, result_bytes));
}

bool WorkReplay::matches_grant_creation(const CommandMapping &mapping, ayni_bytes_v1 canonical,
                                        const authority::Grant &grant, ayni_bytes_v1 domain,
                                        ayni_bytes_v1 scope) {
    AuthorityCommandRecord request{};
    // C parents have creation revision 1 while ACTIVE; later terminal
    // revisions must not rewrite the original command's expected revision.
    return grant.bound_goal_id == 0U && read_authority_command(canonical, request) &&
           grant.created_stamp.commit_seq == mapping.commit_seq &&
           mapping.principal_id == grant.owner_principal_id &&
           mapping.object.id == grant.grant_id &&
           grant.capability_id.equals(request.capability_id) &&
           grant.capability_version == request.capability_version &&
           grant.subject_instance_id == request.subject_instance_id &&
           grant.parent_grant_id == request.parent_grant_id && grant.lifetime == request.lifetime &&
           grant.duration_us == request.duration_us &&
           grant.allow_delegation == ((request.flags & AYNI_GRANT_ALLOW_DELEGATION) != 0U) &&
           request.expected_revision ==
               (grant.parent_grant_id == 0U ? 0U : authority::grant_active_revision) &&
           authority_command_scope_matches(request, domain, scope);
}

// Selected revisions describe past admission. A later terminal Grant revision
// is legal; current consumption is a separate check. Only durably terminal work
// may keep history whose Grant row has already been reclaimed (ADR-0037).
bool WorkReplay::goal_authority_consistent(uint32_t goal_index, uint64_t boot_epoch_limit) const {
    const auto &goal = model_.goals.at(goal_index);
    if (!model_.goal_authority.contains(goal_index, goal.goal_id)) {
        return false; // Schema 2 never infers authorization from absent evidence.
    }
    const auto record = model_.goal_authority.at(goal_index);
    if (record.stamp.commit_seq < goal.created.commit_seq ||
        record.stamp.transition_id < goal.created.transition_id) {
        return false;
    }
    if (record.phase != work::GoalAuthorityPhase::proposed &&
        (record.binding.parent.goal_id != goal.parent_goal_id ||
         record.binding.established_seq < goal.created.commit_seq ||
         record.binding.input.incarnation > boot_epoch_limit ||
         (record.binding.established_seq == goal.created.commit_seq &&
          record.binding.input.incarnation != 0U))) {
        return false;
    }
    if (goal.parent_index != work::GoalTable::no_parent) {
        const auto parent = model_.goal_authority.at(goal.parent_index);
        if (!work::goal_ceiling_covers(parent, record) ||
            (record.phase != work::GoalAuthorityPhase::proposed &&
             (record.binding.parent.commit_seq > parent.binding.established_seq ||
              record.binding.parent.commit_seq <
                  model_.goals.at(goal.parent_index).created.commit_seq))) {
            return false;
        }
    }
    const bool live = work::GoalTable::is_live(goal.lifecycle);
    if (foundation::id_boot_epoch(record.goal_id) > boot_epoch_limit ||
        (live && ((record.phase == work::GoalAuthorityPhase::proposed) !=
                  (goal.lifecycle == AYNI_GOAL_PROPOSED)))) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &requirement = record.requirements[index];
        const auto &reference = requirement.dependency;
        if (reference.grant_id == 0U) {
            continue; // The shared validator permits missing identities only in PROPOSED.
        }
        if (foundation::id_boot_epoch(reference.grant_id) > boot_epoch_limit ||
            reference.revision != authority::grant_active_revision) {
            return false;
        }
        uint32_t grant_index = 0;
        if (!model_.grants.find(reference.grant_id, grant_index)) {
            if (live) {
                return false;
            }
            continue;
        }
        const auto &grant = model_.grants.at(grant_index);
        if (grant.created_stamp.transition_id > record.stamp.transition_id ||
            !matches_goal_selection(
                goal_index, requirement, grant_index,
                record.binding.established_seq == 0U ? record.stamp.commit_seq
                                                     : record.binding.established_seq,
                record.binding.input.incarnation == 0U ? SelectionOrigin::approval
                                                       : SelectionOrigin::existing_grant)) {
            return false;
        }
    }
    return true;
}

bool WorkReplay::calendars_consistent() const {
    for (uint32_t index = 0; index < model_.goals.size(); ++index) {
        const auto &goal = model_.goals.at(index);
        const auto &row = model_.calendars.at(index);
        if (work::is_calendar_trigger(goal.trigger.kind)) {
            if (row.goal_id != goal.goal_id || row.taken != goal.cursor.taken ||
                work::calendar_kind(goal.trigger.kind) != row.rule.kind) {
                return false;
            }
        } else if (row.goal_id != 0) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        const auto &task = model_.tasks.at(index);
        const auto &row = model_.calendars.at(task.goal_index);
        if (work::is_calendar_trigger(model_.goals.at(task.goal_index).trigger.kind)) {
            if (task.occurrence > row.taken ||
                !work::calendar_occurrence_matches(row.rule, task.occurrence, task.calendar_local_s,
                                                   task.calendar_utc_s)) {
                return false;
            }
        } else if (task.calendar_local_s != AYNI_CALENDAR_TIME_NONE ||
                   task.calendar_utc_s != AYNI_CALENDAR_TIME_NONE) {
            return false;
        }
    }
    return true;
}

// Ending a goal ends its descendants and every task it reaches, and
// stopping an instance ends every goal it owns, in the same transition
// (ADR-0018, ADR-0025 decision 5); so no stored transition leaves a live
// goal under a terminal parent or a stopped owner, or a live task under a
// terminal goal.
bool WorkReplay::consistent(uint64_t boot_epoch_limit) const {
    for (uint32_t at = 0; at < model_.scenes.state_count(); ++at) {
        const auto id = model_.scenes.state(at).object_id;
        uint32_t index = 0;
        if (foundation::id_boot_epoch(id) > boot_epoch_limit || model_.goals.find(id, index) ||
            model_.tasks.find(id, index) || model_.grants.find(id, index) ||
            model_.calls.find(id, index) || model_.agents.find_instance(id) != nullptr) {
            return false;
        }
    }
    if (!calendars_consistent() || !model_.provenance.complete() ||
        !provenance_consistent(boot_epoch_limit) || !contexts_consistent(boot_epoch_limit) ||
        !model_.grants.consistent() || !calls_consistent(boot_epoch_limit) ||
        !memory_identities_consistent(boot_epoch_limit)) {
        return false;
    }
    const auto &cleanup = model_.authority_cleanup.record();
    if (foundation::id_boot_epoch(cleanup.after_goal_id) > boot_epoch_limit ||
        foundation::id_boot_epoch(cleanup.through_goal_id) > boot_epoch_limit ||
        foundation::id_boot_epoch(cleanup.requested_through) > boot_epoch_limit) {
        return false;
    }
    const auto *binding = model_.principals.binding();
    for (uint32_t index = 0; index < model_.grants.size(); ++index) {
        const auto &grant = model_.grants.at(index);
        if (grant.lifecycle == AYNI_GRANT_GOAL_ENDED) {
            uint32_t goal_index = 0;
            if (model_.goals.find(grant.bound_goal_id, goal_index) &&
                (work::GoalTable::is_live(model_.goals.at(goal_index).lifecycle) ||
                 model_.goals.at(goal_index).commit_seq >= grant.stamp.commit_seq)) {
                return false;
            }
        }
        if (binding == nullptr || binding->principal_id != grant.owner_principal_id ||
            binding->binding_revision != grant.owner_binding_revision ||
            model_.agents.find_instance(grant.subject_instance_id) == nullptr ||
            foundation::id_boot_epoch(grant.grant_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(grant.subject_instance_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(grant.bound_goal_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(grant.revoked_by_root_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(grant.cleanup_after_grant_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(grant.cleanup_through_grant_id) > boot_epoch_limit) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.goals.size(); ++index) {
        const work::Goal &goal = model_.goals.at(index);
        if (!goal_authority_consistent(index, boot_epoch_limit) ||
            (goal.parent_index != work::GoalTable::no_parent &&
             model_.goals.at(goal.parent_index).lifecycle == AYNI_GOAL_PROPOSED)) {
            return false;
        }
        if (!work::GoalTable::is_live(goal.lifecycle)) {
            continue;
        }
        const work::AgentInstance *owner = model_.agents.find_instance(goal.owner_instance_id);
        if ((goal.parent_index != work::GoalTable::no_parent &&
             !work::GoalTable::is_live(model_.goals.at(goal.parent_index).lifecycle)) ||
            owner == nullptr || owner->lifecycle == AYNI_AGENT_STOPPED) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        const work::Task &task = model_.tasks.at(index);
        if (!task_authority_consistent(index, boot_epoch_limit)) {
            return false;
        }
        if (model_.goals.at(task.goal_index).lifecycle == AYNI_GOAL_PROPOSED ||
            (work::TaskTable::is_live(task.lifecycle) && task.lifecycle != AYNI_TASK_CANCELLING &&
             !work::GoalTable::is_live(model_.goals.at(task.goal_index).lifecycle))) {
            return false;
        }
    }
    return true;
}

void WorkReplay::finish(uint64_t now_us) {
    model_.agents.fault_unregistered();
    model_.goals.resolve_types([this](const catalog::CatalogId &id, uint32_t version,
                                      ayni_trigger_kind_t trigger,
                                      const catalog::WorkProfile &profile, uint32_t &out_index) {
        const catalog::GoalTypeRegistry::Lookup type =
            model_.catalogs.goal_types.find({id.data(), id.size()}, version);
        switch (type.kind) {
        case catalog::GoalTypeRegistry::Lookup::Kind::found:
            if (!(model_.catalogs.goal_types.at(type.index).work_profile == profile)) {
                return AYNI_REASON_GOAL_TYPE_VERSION;
            }
            out_index = type.index;
            // A goal with a trigger derives tasks, and the type no longer
            // has a strategy to decide them.
            return trigger != AYNI_TRIGGER_NONE &&
                           model_.catalogs.goal_types.at(type.index).strategy_index ==
                               catalog::GoalType::no_strategy
                       ? AYNI_REASON_STRATEGY_CHANGED
                       : AYNI_REASON_NONE;
        case catalog::GoalTypeRegistry::Lookup::Kind::unknown_version:
            return AYNI_REASON_GOAL_TYPE_VERSION;
        case catalog::GoalTypeRegistry::Lookup::Kind::unknown_id:
            break;
        }
        return AYNI_REASON_GOAL_TYPE_UNKNOWN;
    });
    model_.grants.resolve_catalog(model_.catalogs.capabilities);
    for (uint32_t index = 0; index < model_.grants.size(); ++index) {
        const auto *subject =
            model_.agents.find_instance(model_.grants.at(index).subject_instance_id);
        if (!work::AgentTable::usable(subject)) {
            model_.grants.hold_restored(index, AYNI_REASON_GRANT_SUBJECT);
        }
    }
    hold_tasks();
    model_.goals.reanchor(now_us);
    model_.tasks.reanchor(now_us);
    model_.forget_due_work(); // Reanchoring and the holds above take no transition.
}

// The strategy that decides a task is its goal type's, and only the
// strategy that wrote a checkpoint, at that version, can read it
// (C-POLICY-010).
void WorkReplay::hold_tasks() {
    work::TaskTable &tasks = model_.tasks;
    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const work::Task &task = tasks.at(index);
        if (!work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        ayni_reason_t goal_hold = AYNI_REASON_NONE;
        // A compatible child still depends on its ancestors' types. Keep this
        // inherited reason volatile, so startup cannot rewrite the Task into a
        // user pause or advance its revision just for compatibility (C-LIFE-024).
        const bool compatible = model_.goals.visit_lineage(task.goal_index, [&](uint32_t at) {
            goal_hold = model_.goals.at(at).hold_reason;
            return goal_hold == AYNI_REASON_NONE;
        });
        if (!compatible) {
            tasks.hold_restored(index,
                                goal_hold == AYNI_REASON_GOAL_TYPE_UNKNOWN
                                    ? AYNI_NOT_FOUND
                                    : AYNI_UNSUPPORTED_VERSION,
                                goal_hold);
            continue;
        }
        const work::Goal &goal = model_.goals.at(task.goal_index);
        const catalog::GoalType &type = model_.catalogs.goal_types.at(goal.goal_type_index);
        if (type.strategy_index == catalog::GoalType::no_strategy) {
            // A goal without a trigger derives no task, and one with a
            // trigger is held above, so only a log this library did not
            // write gets here; hold rather than ask a strategy that is not
            // there.
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
            continue;
        }
        const catalog::Strategy &strategy = model_.catalogs.strategies.at(type.strategy_index);
        if (task.checkpoint_writer_version == 0U) {
            continue;
        }
        if (!strategy.accepts_checkpoint(
                {task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size()},
                task.checkpoint_writer_version)) {
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
        } else if (task.checkpoint_size > strategy.max_checkpoint_bytes) {
            tasks.hold_restored(index, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT);
        }
    }
}

void WorkReplay::discard() {
    model_.principals.drop_restored_binding();
    model_.agents.drop_restored();
    model_.goals.clear();
    model_.calendars.clear();
    model_.goal_authority.clear_all();
    model_.task_authority.clear_all();
    model_.authority_cleanup.clear();
    model_.tasks.clear();
    model_.calls.clear();
    model_.memories.clear();
    model_.provenance.clear();
    model_.privacy.clear();
    model_.contexts.clear();
    model_.commands.clear_restored();
    model_.grants.clear_restored();
    model_.scenes.clear_states();
    batch_seq_ = 0;
    pairing_ = {};
}

void WorkReplay::discard_for_reload() {
    discard();
    model_.sessions.clear();
    model_.forget_due_work();
    model_.ids = foundation::IdAllocator{};
    model_.transitions = TransitionClock{};
}

} // namespace ayni::kernel
