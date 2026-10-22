#include "kernel/intent/work_model.h"

namespace ayni::kernel {

bool WorkModel::start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp) {
    ids.start(boot_epoch);
    transitions.start(first_stamp);
    const bool starts = agents.has_registered();
    const bool binds = principals.needs_binding();
    work::WorkTables work = tables();
    if (!starts && !binds && !work::settles_restored(work)) {
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
    return true;
}

ayni_status_t open_session(WorkModel &model, uint64_t principal_id, ayni_channel_t channel,
                           uint64_t &out_session_id) {
    if (model.principals.find(principal_id) == nullptr) {
        return AYNI_NOT_FOUND;
    }
    uint64_t session_id = 0;
    if (model.sessions.full() || !model.ids.allocate(session_id)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    // Cannot fail: the table has room.
    static_cast<void>(model.sessions.open({session_id, principal_id, channel,
                                           model.principals.find(principal_id)->binding_revision}));
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
    if (!model.goals.takes_input(source)) {
        return false;
    }
    model.goals.record_input(source, now_us, model.transitions.next());
    model.due_work_changed = true;
    return true;
}

bool refresh_due_work(WorkModel &model, uint64_t now_us, uint64_t &out_next_us) {
    work::DueWork work{};
    const bool due = next_due_work(model, now_us, work);
    out_next_us = due ? now_us : model.next_due_work_us;
    return due;
}

bool do_due_work(WorkModel &model, uint64_t now_us) {
    work::DueWork work{};
    if (!next_due_work(model, now_us, work)) {
        return false;
    }
    work::WorkTables tables = model.tables();
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
