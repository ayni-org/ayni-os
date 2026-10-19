#include "kernel/intent/work_model.h"

namespace ayni::kernel {

void WorkModel::start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp) {
    ids.start(boot_epoch);
    transitions.start(first_stamp);
    if (agents.has_registered()) {
        agents.start_all(ids, transitions.next());
    }
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
    static_cast<void>(model.sessions.open({session_id, principal_id, channel}));
    out_session_id = session_id;
    return AYNI_OK;
}

namespace {

// Clears derivation_changed when there is nothing to derive, so later calls
// skip the search until a change sets it again or next_due_us comes. Keeps it
// set when a goal is found: deriving from it changes the tables.
bool next_derivable(WorkModel &model, uint64_t now_us, uint32_t &out_goal_index) {
    if (!model.derivation_changed && now_us < model.next_due_us) {
        return false;
    }
    if (!model.ids.available()) {
        model.derivation_changed = false;
        model.next_due_us = work::no_time;
        return false;
    }
    uint64_t next_due_us = work::no_time;
    if (work::find_derivable(model.tables(), now_us, out_goal_index, next_due_us)) {
        model.derivation_changed = true;
        return true;
    }
    model.derivation_changed = false;
    model.next_due_us = next_due_us;
    return false;
}

} // namespace

bool refresh_derivation_due(WorkModel &model, uint64_t now_us, uint64_t &out_next_due_us) {
    uint32_t goal_index = 0;
    const bool due = next_derivable(model, now_us, goal_index);
    out_next_due_us = due ? now_us : model.next_due_us;
    return due;
}

bool derive_next_task(WorkModel &model, uint64_t now_us) {
    uint32_t goal_index = 0;
    if (!next_derivable(model, now_us, goal_index)) {
        return false;
    }
    uint64_t task_id = 0;
    static_cast<void>(model.ids.allocate(task_id)); // available() above
    work::WorkTables tables = model.tables();
    work::derive(tables, goal_index, now_us, task_id, model.transitions.next());
    return true;
}

} // namespace ayni::kernel
