#include "kernel/service/model/work_model.h"

namespace ayni::kernel {

// C-AUTH-019/024/032: both authority commands require the current durable owner
// binding and an authenticated management channel, independent of the payload.
bool bound_management_session(const WorkModel &model, const authority::Session &session,
                              uint64_t durable_seq) {
    const auto *binding = model.principals.binding();
    return session.channel == AYNI_CHANNEL_MANAGEMENT && binding != nullptr &&
           binding->principal_id == session.principal_id &&
           binding->binding_revision == session.binding_revision &&
           binding->stamp.commit_seq <= durable_seq;
}

bool WorkModel::retains_task(const void *context, const work::Task &task) {
    const auto &model = *static_cast<const WorkModel *>(context);
    // Known terminal facts are encoded before a later replacement transition.
    // Unknown or pending calls retain the Task even after local cancellation.
    return !model.calls.can_retire_task(task.task_id, UINT64_MAX);
}

bool WorkModel::retains_goal(const void *context, const work::Goal &goal) {
    const auto &model = *static_cast<const WorkModel *>(context);
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        const auto &task = model.tasks.at(index);
        if (task.goal_id == goal.goal_id &&
            (work::TaskTable::is_live(task.lifecycle) || retains_task(context, task))) {
            return true;
        }
    }
    return false;
}

bool WorkModel::start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp) {
    ids.start(boot_epoch);
    transitions.start(first_stamp);
    const bool starts = agents.has_registered();
    const bool binds = principals.needs_binding();
    work::WorkTables work = tables();
    const bool expires = grants.has_old_boot_expiry(boot_epoch);
    if (!starts && !binds && !expires && !work::settles_restored(work)) {
        return false;
    }
    const foundation::TransitionStamp stamp = transitions.next();
    if (binds) {
        static_cast<void>(principals.bind(stamp));
    }
    if (starts) {
        agents.start_all(ids, stamp);
    }
    work::settle_restored(work, stamp);
    grants.expire_old_boot(boot_epoch, stamp);
    // Restored scan progress remains intact. Changed authority/terminal facts
    // request another pass when this startup transition is journaled.
    return true;
}

// The Grant owner checks attenuation and time; Kernel owns the subjects
// that complete the usable chain, including ancestors with other subjects.
authority::GrantCheck WorkModel::check_grant(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                             uint64_t goal_id) const {
    authority::GrantTable::CheckedChain chain{};
    const auto check =
        grants.check_chain(index, {ids.boot_epoch(), now_us, durable_seq, goal_id}, chain);
    if (check.status == AYNI_OK && grants.at(index).bound_goal_id != 0U) {
        uint32_t goal_index = 0;
        if (!goals.find(goal_id, goal_index) ||
            !work::GoalTable::is_live(goals.at(goal_index).lifecycle) ||
            goals.at(goal_index).owner_instance_id != grants.at(index).subject_instance_id) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_GOAL};
        }
    }
    return check.status == AYNI_OK ? check_grant_subjects(chain) : check;
}

// The Grant owner already proved every link/time/catalog condition. Checking
// only work subjects preserves owner direction and the original error order.
authority::GrantCheck
WorkModel::check_grant_subjects(const authority::GrantTable::CheckedChain &chain) const {
    for (uint32_t index = 0; index < chain.count; ++index) {
        const auto &grant = grants.at(chain.indices[index]);
        if (!work::AgentTable::usable(agents.find_instance(grant.subject_instance_id))) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_SUBJECT};
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Current references are facts owned by work. Authority receives only a local
// exclusion mask, so its reclaim policy never reads Goal state (ADR-0037).
authority::GrantReclaimExclusions WorkModel::grant_reclaim_exclusions(uint64_t durable_seq) const {
    authority::GrantReclaimExclusions excluded;
    for (uint32_t index = 0; index < goals.size(); ++index) {
        const auto &goal = goals.at(index);
        if ((!work::GoalTable::is_live(goal.lifecycle) && goal.commit_seq <= durable_seq) ||
            !goal_authority.contains(index, goal.goal_id)) {
            continue;
        }
        const auto record = goal_authority.at(index);
        for (uint32_t requirement = 0; requirement < record.count; ++requirement) {
            uint32_t grant_index = 0;
            if (grants.find(record.requirements[requirement].dependency.grant_id, grant_index)) {
                excluded.add(grant_index);
            }
        }
    }
    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const auto &task = tasks.at(index);
        work::TaskAuthorityRecord record{};
        if ((!work::TaskTable::is_live(task.lifecycle) && task.commit_seq <= durable_seq &&
             calls.can_retire_task(task.task_id, durable_seq)) ||
            !task_authority.read(task.slot, task.task_id, record)) {
            continue;
        }
        for (uint32_t at = 0; at < record.count; ++at) {
            uint32_t grant_index = 0;
            if (grants.find(record.references[at].grant_id, grant_index)) {
                excluded.add(grant_index);
            }
        }
    }
    protect_content_grants(excluded);
    return excluded;
}

void WorkModel::release_goal_references(const work::GoalAuthorityRecord &record,
                                        uint64_t commit_seq) {
    for (uint32_t index = 0; index < record.count; ++index) {
        grants.release_reference(record.requirements[index].dependency.grant_id, commit_seq);
    }
}

void WorkModel::clear_goal_authority(uint32_t index, uint64_t previous_id, uint64_t commit_seq) {
    work::GoalAuthorityRecord record{};
    if (goal_authority.read(index, previous_id, record)) {
        release_goal_references(record, commit_seq);
        goal_authority.clear(index);
    }
}

bool WorkModel::insert_goal(const work::NewGoal &goal, uint64_t goal_id,
                            foundation::TransitionStamp stamp, uint32_t &out_index) {
    const auto replacing = goals.giving_way();
    if (replacing != work::GoalTable::no_index) {
        // All creation admission has passed; terminal Tasks leave with this exact Goal.
        retire_goal_task_metadata(replacing, stamp.commit_seq);
    }
    auto work = tables();
    if (!work::insert_goal(work, goal, goal_id, stamp, out_index)) {
        return false;
    }
    clear_goal_authority(out_index, goals.at(out_index).replaced_goal_id, stamp.commit_seq);
    clear_content_provenance(work::ProvenanceKind::goal, out_index,
                             goals.at(out_index).replaced_goal_id, stamp.commit_seq);
    return true;
}

ayni_status_t open_session(WorkModel &model, const authority::Principal &principal,
                           ayni_channel_t channel, uint64_t &out_session_id) {
    uint64_t session_id = 0;
    if (model.sessions.full() || !model.ids.allocate(session_id)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    // Cannot fail: the table has room.
    static_cast<void>(model.sessions.open(
        {session_id, principal.principal_id, channel, principal.binding_revision}));
    out_session_id = session_id;
    return AYNI_OK;
}

// Every change to the tables a search reads takes a transition, so the next
// commit_seq keys the kept search; nothing that changes them has to mark it.
// Ids are compared too, because whatever takes the last one (a goal, a
// session) need not know about due work.
bool WorkModel::next_due_work(uint64_t now_us, uint64_t durable_seq, work::DueWork &out_work,
                              uint64_t &out_next_us) {
    const bool id_left = ids.available();
    const uint64_t commit_seq = transitions.next_commit_seq();
    if (due_search_kept_ && commit_seq == due_search_commit_seq_ &&
        durable_seq == due_search_durable_seq_ && id_left == due_search_with_id_ &&
        now_us < next_due_work_us_) {
        out_next_us = next_due_work_us_;
        return false;
    }
    uint64_t next_us = work::no_time;
    due_search_kept_ =
        !work::find_due_work(tables(), now_us, durable_seq, id_left, out_work, next_us);
    if (!due_search_kept_) {
        out_next_us = now_us;
        return true;
    }
    due_search_commit_seq_ = commit_seq;
    due_search_durable_seq_ = durable_seq;
    due_search_with_id_ = id_left;
    next_due_work_us_ = next_us;
    out_next_us = next_us;
    return false;
}

bool apply_local_input(WorkModel &model, const catalog::CatalogId &source, uint64_t now_us,
                       work::InputResumeBoundary input) {
    if (!model.goals.takes_input(source, &model.goal_authority, input)) {
        return false;
    }
    model.goals.record_input(source, now_us, model.transitions.next(), &model.goal_authority,
                             input);
    return true;
}

bool refresh_due_work(WorkModel &model, uint64_t now_us, uint64_t &out_next_us,
                      uint64_t durable_seq) {
    work::DueWork work{};
    const bool due = model.next_due_work(now_us, durable_seq, work, out_next_us);
    uint32_t index = 0;
    uint64_t deadline = work::no_time;
    if (model.grants.next_expiry({model.ids.boot_epoch(), now_us, 0}, index, deadline)) {
        if (deadline <= now_us) {
            out_next_us = now_us;
            return true;
        }
        if (deadline < out_next_us) {
            out_next_us = deadline;
        }
    }
    return due;
}

DueWorkResult do_due_work(WorkModel &model, uint64_t now_us, uint64_t authority_now_us,
                          uint64_t durable_seq) {
    uint32_t grant_index = 0;
    uint64_t deadline = work::no_time;
    if (model.grants.next_expiry({model.ids.boot_epoch(), now_us, 0}, grant_index, deadline) &&
        deadline <= now_us) {
        model.grants.expire(grant_index, model.transitions.next());
        return DueWorkResult::applied;
    }
    work::DueWork work{};
    uint64_t next_us = work::no_time;
    if (!model.next_due_work(now_us, durable_seq, work, next_us)) {
        return DueWorkResult::none;
    }
    work::WorkTables tables = model.tables();
    if (work.kind != work::DueWork::Kind::expire) {
        const auto check =
            model.check_goal_authority(work.goal_index, authority_now_us, durable_seq);
        if (check.status != AYNI_OK) {
            // Confirmation may still arrive; it must not become a sticky pause.
            if (check.status == AYNI_BUSY) {
                return DueWorkResult::none;
            }
            model.pause_goal_authority(work.goal_index);
            return DueWorkResult::applied;
        }
    }
    switch (work.kind) {
    case work::DueWork::Kind::expire:
        if (work::goal_end_exhausts(tables, work.goal_index, work::GoalChange::expire)) {
            model.goals.drop_expiry(work.goal_index, model.transitions.next());
        } else {
            work::apply_goal_change(tables, work.goal_index, work::GoalChange::expire,
                                    model.transitions.next());
        }
        return DueWorkResult::applied;
    case work::DueWork::Kind::derive: {
        uint64_t task_id = 0;
        static_cast<void>(model.ids.allocate(task_id)); // find_due_work saw one left
        const auto stamp = model.transitions.next();
        work::derive(tables, work.goal_index, now_us, task_id, stamp);
        if (!model.inherit_task_metadata(task_id, stamp)) {
            return DueWorkResult::internal;
        }
        return DueWorkResult::applied;
    }
    case work::DueWork::Kind::skip:
        work::skip(tables, work.goal_index, now_us, work.skip_reason, model.transitions.next());
        return DueWorkResult::applied;
    }
    return DueWorkResult::none;
}

uint32_t WorkModel::accepted_step_count(uint32_t task_index) const {
    const auto &task = tasks.at(task_index);
    uint32_t count = task.step_count;
    for (uint32_t index = 0; index < calls.capacity(); ++index) {
        if (!calls.occupied(index) || calls.identity(index).task_id != task.task_id) {
            continue;
        }
        const auto &key = calls.identity(index).step_key;
        if (!tasks.has_step(task_index, {key.data(), key.size()})) {
            ++count;
        }
    }
    return count;
}

bool WorkModel::step_taken(uint32_t task_index, ayni_string_v1 key) const {
    uint32_t call_index = 0;
    return tasks.has_step(task_index, key) ||
           calls.find_step(tasks.at(task_index).task_id, key, call_index);
}

} // namespace ayni::kernel
