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

} // namespace ayni::kernel
