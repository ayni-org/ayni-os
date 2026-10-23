#include "kernel/intent/work_model.h"

namespace ayni::kernel {

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
    auto work = tables();
    if (!work::insert_goal(work, goal, goal_id, stamp, out_index)) {
        return false;
    }
    clear_goal_authority(out_index, goals.at(out_index).replaced_goal_id, stamp.commit_seq);
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

namespace {

// Clears due_work_changed when nothing is due, so later calls skip the
// search until a change sets it again, next_due_work_us comes or the ids run
// out. Keeps it set when work is found: doing it changes the tables. Ids are
// compared rather than marked because whatever takes the last one (a goal, a
// session) need not know about due work.
bool next_due_work(WorkModel &model, uint64_t now_us, work::DueWork &out_work) {
    const bool id_left = model.ids.available();
    if (!model.due_work_changed && id_left == model.searched_with_id &&
        now_us < model.next_due_work_us) {
        return false;
    }
    model.searched_with_id = id_left;
    uint64_t next_us = work::no_time;
    if (work::find_due_work(model.tables(), now_us, id_left, out_work, next_us)) {
        model.due_work_changed = true;
        return true;
    }
    model.due_work_changed = false;
    model.next_due_work_us = next_us;
    return false;
}

} // namespace

bool apply_local_input(WorkModel &model, const catalog::CatalogId &source, uint64_t now_us) {
    if (!model.goals.takes_input(source, &model.goal_authority)) {
        return false;
    }
    model.goals.record_input(source, now_us, model.transitions.next(), &model.goal_authority);
    model.due_work_changed = true;
    return true;
}

bool refresh_due_work(WorkModel &model, uint64_t now_us, uint64_t &out_next_us) {
    work::DueWork work{};
    const bool due = next_due_work(model, now_us, work);
    out_next_us = due ? now_us : model.next_due_work_us;
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

bool do_due_work(WorkModel &model, uint64_t now_us, uint64_t authority_now_us,
                 uint64_t durable_seq) {
    uint32_t grant_index = 0;
    uint64_t deadline = work::no_time;
    if (model.grants.next_expiry({model.ids.boot_epoch(), now_us, 0}, grant_index, deadline) &&
        deadline <= now_us) {
        model.grants.expire(grant_index, model.transitions.next());
        return true;
    }
    work::DueWork work{};
    if (!next_due_work(model, now_us, work)) {
        return false;
    }
    work::WorkTables tables = model.tables();
    if (work.kind != work::DueWork::Kind::expire) {
        const auto check =
            model.check_goal_authority(work.goal_index, authority_now_us, durable_seq);
        if (check.status != AYNI_OK) {
            // Confirmation may still arrive; it must not become a sticky pause.
            if (check.reason == AYNI_REASON_GRANT_NOT_DURABLE) {
                return false;
            }
            model.pause_goal_authority(work.goal_index);
            return true;
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
        return true;
    case work::DueWork::Kind::derive: {
        uint64_t task_id = 0;
        static_cast<void>(model.ids.allocate(task_id)); // find_due_work saw one left
        work::derive(tables, work.goal_index, now_us, task_id, model.transitions.next());
        return true;
    }
    case work::DueWork::Kind::skip:
        work::skip(tables, work.goal_index, now_us, work.skip_reason, model.transitions.next());
        return true;
    }
    return false;
}

} // namespace ayni::kernel
