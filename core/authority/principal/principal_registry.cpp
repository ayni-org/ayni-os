#include "authority/principal/principal_registry.h"

namespace ayni::authority {

bool PrincipalRegistry::add_owner(const Principal &principal) {
    if (has_owner_) {
        return false;
    }
    owner_ = principal;
    has_owner_ = true;
    return true;
}

const Principal *PrincipalRegistry::find(uint64_t principal_id) const {
    return has_owner_ && owner_.principal_id == principal_id ? &owner_ : nullptr;
}

bool SessionTable::close(uint64_t session_id) {
    for (uint32_t index = 0; index < sessions_.size(); ++index) {
        if (sessions_[index].session_id == session_id) {
            sessions_.remove_at(index);
            return true;
        }
    }
    return false;
}

const Session *SessionTable::find(uint64_t session_id) const {
    for (uint32_t index = 0; index < sessions_.size(); ++index) {
        if (sessions_[index].session_id == session_id) {
            return &sessions_[index];
        }
    }
    return nullptr;
}

} // namespace ayni::authority
