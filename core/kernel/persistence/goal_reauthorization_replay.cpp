// A replacement is one sealed Goal/direct-live-Task/command/footer batch.
// Table owners validate their facts; this proof owns only cross-table pairing.
#include "kernel/persistence/work_replay.h"

#include "foundation/kernel_id.h"
#include "kernel/command/command_records.h"
#include "work/authority/authority_cleanup_records.h"
#include "work/goal/goal_authority_records.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {

// Other state changes could shrink or alter the promised live cohort; only
// the ordered replacement partners belong between its Goal state and footer.
bool WorkReplay::allows_reauthorization_record(const durability::RecordHeader &header) const {
    if (!pairing_.reauthorization) {
        return true;
    }
    using Stage = BatchPairing::ReauthorizationPair::Stage;
    switch (pairing_.reauthorization->stage) {
    case Stage::state:
        return header.type == work::goal_authority_record_type &&
               header.version == work::goal_authority_reauthorization_record_version;
    case Stage::metadata:
        return (header.type == work::task_state_record_type &&
                work::task_state_base_version(header.version) ==
                    work::task_state_reauthorization_record_version) ||
               header.type == work::task_authority_record_type ||
               header.type == command_record_type;
    case Stage::command:
        return header.type == work::authority_cleanup_record_type;
    }
    return false;
}

bool WorkReplay::begin_reauthorization(uint32_t index, foundation::TransitionStamp stamp) {
    const auto &goal = model_.goals.at(index);
    work::GoalAuthorityRecord previous{};
    if (batch_seq_ == stamp.commit_seq || pairing_.reauthorization || pairing_.approval ||
        !pairing_.partners_complete() || !work::GoalTable::is_live(goal.lifecycle) ||
        goal.lifecycle == AYNI_GOAL_PROPOSED || goal.commit_seq >= stamp.commit_seq ||
        !model_.goal_authority.read(index, goal.goal_id, previous) ||
        previous.phase == work::GoalAuthorityPhase::proposed) {
        return false;
    }
    pairing_.reauthorization =
        BatchPairing::ReauthorizationPair{BatchPairing::ReauthorizationPair::Stage::state,
                                          goal.goal_id,
                                          index,
                                          goal.revision,
                                          previous.stamp.commit_seq,
                                          previous.binding.established_seq};
    pairing_.goal_authority = goal.goal_id;
    pairing_.work_cleanup.require();
    return true;
}

// Before replacing metadata, check every old live admission against its old
// ceiling. Missing evidence cannot be silently filled by the replacement.
bool WorkReplay::pair_reauthorization_metadata(uint32_t index,
                                               const work::GoalAuthorityRecord &record,
                                               foundation::TransitionStamp stamp) {
    if (!pairing_.reauthorization) {
        return false;
    }
    auto &pair = *pairing_.reauthorization;
    if (pair.stage != BatchPairing::ReauthorizationPair::Stage::state || index != pair.goal_index ||
        record.phase != work::GoalAuthorityPhase::authorized ||
        record.binding.input.incarnation == 0U ||
        !matches_goal_binding(index, record, stamp.commit_seq) ||
        model_.check_reauthorization_tasks(index).status != AYNI_OK) {
        return false;
    }
    pair.stage = BatchPairing::ReauthorizationPair::Stage::metadata;
    return true;
}

// No live direct Task may escape the batch. Terminal history and children
// retain their original facts and are outside this replacement cohort.
bool WorkReplay::reauthorized_cohort_complete(uint32_t index,
                                              foundation::TransitionStamp stamp) const {
    const auto goal_id = model_.goals.at(index).goal_id;
    for (uint32_t at = 0; at < model_.tasks.size(); ++at) {
        const auto &task = model_.tasks.at(at);
        if (task.goal_id != goal_id || !work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        work::TaskAuthorityRecord admission{};
        if (task.commit_seq != stamp.commit_seq ||
            !model_.task_authority.read(task.slot, task.task_id, admission) ||
            admission.phase != work::TaskAuthorityPhase::authorized ||
            admission.stamp.commit_seq != stamp.commit_seq ||
            admission.stamp.transition_id != stamp.transition_id ||
            !model_.matches_task_ceiling(admission, index)) {
            return false;
        }
    }
    return true;
}

bool WorkReplay::pair_reauthorization_command(const CommandMapping &mapping,
                                              ayni_bytes_v1 canonical, ayni_bytes_v1 result_bytes) {
    ReauthorizationCommand request{};
    const auto *owner = model_.principals.binding();
    if (!pairing_.reauthorization || !read_reauthorization_command(canonical, request) ||
        owner == nullptr || mapping.principal_id != owner->principal_id ||
        owner->stamp.commit_seq >= mapping.commit_seq) {
        return false;
    }
    auto &pair = *pairing_.reauthorization;
    const auto &goal = model_.goals.at(pair.goal_index);
    const auto record = model_.goal_authority.at(pair.goal_index);
    if (pair.stage != BatchPairing::ReauthorizationPair::Stage::metadata ||
        mapping.object.id != pair.goal_id || request.expected_revision != pair.revision ||
        request.reauthorization.expected_authority_commit_seq != pair.authority_seq ||
        !matches_goal_creation(request.reauthorization.original, goal,
                               model_.goals.parameters(pair.goal_index)) ||
        !work_authority_result_matches(record, result_bytes) ||
        !reauthorized_cohort_complete(pair.goal_index, record.stamp)) {
        return false;
    }
    pair.stage = BatchPairing::ReauthorizationPair::Stage::command;
    return true;
}

// Snapshot retention may outlive replaced Grant rows. Retained rows must
// still prove the exact original ceiling and authority at this result's time.
bool WorkReplay::matches_goal_reauthorization(const CommandMapping &mapping,
                                              ayni_bytes_v1 canonical, ayni_bytes_v1 result_bytes,
                                              uint64_t transition_limit,
                                              uint64_t boot_epoch_limit) const {
    ReauthorizationCommand request{};
    WorkAuthorityResult result{};
    const auto *owner = model_.principals.binding();
    if (!read_reauthorization_command(canonical, request) ||
        !read_work_authority_result(result_bytes, mapping.object.id, result) ||
        !work_authority_result_within(result, transition_limit, boot_epoch_limit) ||
        owner == nullptr || mapping.principal_id != owner->principal_id ||
        owner->stamp.commit_seq >= mapping.commit_seq ||
        model_.agents.find_instance(request.reauthorization.original.owner_instance_id) ==
            nullptr) {
        return false;
    }
    uint32_t index = 0;
    if (!model_.goals.find(mapping.object.id, index)) {
        return true;
    }
    const auto &goal = model_.goals.at(index);
    auto record = model_.goal_authority.at(index);
    if (request.reauthorization.expected_authority_commit_seq < goal.created.commit_seq ||
        goal.created.transition_id >= result.transition_id ||
        goal.revision < mapping.object.revision || goal.commit_seq < mapping.commit_seq ||
        record.stamp.commit_seq < mapping.commit_seq || record.count != result.count ||
        !retains_work_authority_result(record, result) ||
        !matches_goal_creation(request.reauthorization.original, goal,
                               model_.goals.parameters(index))) {
        return false;
    }
    record.binding = result.binding;
    return matches_goal_binding(index, record, mapping.commit_seq, BindingEvidence::historical) &&
           saved_references_consistent(index, record, result.references, mapping.commit_seq);
}

// A terminal Task can retain the old generation after the Goal changes its
// choices. Live Tasks must still match the current ceiling and deny phase.
bool WorkReplay::task_authority_consistent(uint32_t index, uint64_t boot_epoch_limit) const {
    const auto &task = model_.tasks.at(index);
    const auto &goal = model_.goals.at(task.goal_index);
    const auto ceiling = model_.goal_authority.at(task.goal_index);
    work::TaskAuthorityRecord admission{};
    if (!model_.task_authority.read(task.slot, task.task_id, admission) ||
        admission.stamp.commit_seq > task.commit_seq ||
        foundation::id_boot_epoch(task.task_id) > boot_epoch_limit ||
        admission.ceiling.goal_id != task.goal_id || admission.count != ceiling.count) {
        return false;
    }
    if (model_.matches_task_ceiling(admission, task.goal_index)) {
        return !work::TaskTable::is_live(task.lifecycle) ||
               ((admission.phase == work::TaskAuthorityPhase::paused) ==
                (ceiling.phase == work::GoalAuthorityPhase::paused));
    }
    if (work::TaskTable::is_live(task.lifecycle) ||
        admission.ceiling.commit_seq < goal.created.commit_seq ||
        admission.ceiling.commit_seq >= ceiling.binding.established_seq) {
        return false;
    }
    for (uint32_t at = 0; at < admission.count; ++at) {
        uint32_t grant_index = 0;
        const auto &reference = admission.references[at];
        const auto &need = ceiling.requirements[at];
        if (foundation::id_boot_epoch(reference.grant_id) > boot_epoch_limit ||
            reference.revision != authority::grant_active_revision) {
            return false;
        }
        if (model_.grants.find(reference.grant_id, grant_index) &&
            !matches_goal_selection(task.goal_index, need, grant_index,
                                    admission.ceiling.commit_seq, SelectionOrigin::approval)) {
            return false;
        }
    }
    return true;
}

} // namespace ayni::kernel
