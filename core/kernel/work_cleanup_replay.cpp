// Cross-table proof for one bounded work scan request/step (ADR-0038).
// Codecs own representation; work owns scan transitions; Kernel owns pairing.
#include "kernel/work_replay.h"

#include "work/authority/authority_cleanup_records.h"

namespace ayni::kernel {
namespace {

// A new-version Goal pause promises its whole direct live Task cohort. A
// policy/user hold is independent; only the admission's deny phase is required.
bool paused_cohort_complete(const WorkModel &model, uint64_t goal_id) {
    if (goal_id == 0U) {
        return true;
    }
    for (uint32_t at = 0; at < model.tasks.size(); ++at) {
        const auto &task = model.tasks.at(at);
        if (task.goal_id != goal_id || !work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        work::TaskAuthorityRecord record{};
        if (!model.task_authority.read(task.slot, task.task_id, record) ||
            record.phase != work::TaskAuthorityPhase::paused) {
            return false;
        }
    }
    return true;
}

bool matches_cleanup_request(const WorkModel &model, const work::AuthorityCleanupRecord &record) {
    auto requested = model.authority_cleanup;
    requested.request(model.goals.latest_id(), record.stamp);
    return work::same_authority_cleanup_record(requested.record(), record);
}

// Reconstruct either a standalone request or exactly one stable scan item;
// both use the work owner's transitions rather than duplicating cursor math.
bool matches_cleanup_transition(const WorkModel &model, const work::AuthorityCleanupRecord &record,
                                uint64_t paused_goal, uint64_t ended_goal) {
    const bool changed = model.authority_cleanup_change_in(record.stamp);
    if (changed && matches_cleanup_request(model, record)) {
        return ended_goal == 0U;
    }
    if (!model.authority_cleanup.ready()) {
        return false;
    }
    work::AuthorityCleanup stepped = model.authority_cleanup;
    AuthorityCleanupGoal selected{};
    if (model.next_authority_cleanup_goal(selected)) {
        if ((paused_goal != 0U && paused_goal != selected.goal_id) ||
            (ended_goal != 0U && ended_goal != selected.goal_id) ||
            ((selected.index == work::GoalTable::no_index ||
              !work::GoalTable::is_live(model.goals.at(selected.index).lifecycle)) &&
             model.grants.has_active_goal_grants(selected.goal_id))) {
            return false;
        }
        stepped.visited(selected.goal_id, record.stamp);
    } else {
        if (paused_goal != 0U || ended_goal != 0U) {
            return false;
        }
        stepped.finish(record.stamp);
    }
    if (changed) {
        stepped.request(model.goals.latest_id(), record.stamp);
    }
    return work::same_authority_cleanup_record(stepped.record(), record);
}

} // namespace

// A footer is complete only after work/command partners and the direct pause
// cohort. It cannot skip stable IDs, clear pending requests or cross batches.
durability::RecordUse WorkReplay::restore_authority_cleanup(const uint8_t *body, uint32_t size,
                                                            foundation::TransitionStamp stamp) {
    work::AuthorityCleanupRecord record{};
    if (!pairing_.partners_complete() || !work::read_authority_cleanup_record(body, size, record) ||
        record.stamp.transition_id != stamp.transition_id ||
        record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.commit_seq <= model_.authority_cleanup.record().stamp.commit_seq ||
        record.stamp.transition_id <= model_.authority_cleanup.record().stamp.transition_id ||
        !paused_cohort_complete(model_, pairing_.work_cleanup.paused_goal_id) ||
        !(pairing_.reauthorization
              ? matches_cleanup_request(model_, record)
              : matches_cleanup_transition(model_, record, pairing_.work_cleanup.paused_goal_id,
                                           pairing_.work_cleanup.ended_goal_id))) {
        return durability::RecordUse::corrupt;
    }
    model_.authority_cleanup.restore_validated(record);
    pairing_.work_cleanup.stage = BatchPairing::WorkCleanupPair::Stage::complete;
    return durability::RecordUse::applied;
}

} // namespace ayni::kernel
