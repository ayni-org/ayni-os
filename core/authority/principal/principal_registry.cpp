#include "authority/principal/principal_registry.h"

#include "authority/principal/owner_binding_records.h"

namespace ayni::authority {

bool PrincipalRegistry::add_owner(const Principal &principal) {
    if (has_owner_ || principal.principal_id == 0U || principal.binding_revision == 0U ||
        principal.kind != AYNI_PRINCIPAL_OWNER) {
        return false;
    }
    owner_ = principal;
    has_owner_ = true;
    return true;
}

const Principal *PrincipalRegistry::find(uint64_t principal_id) const {
    return has_owner_ && owner_.principal_id == principal_id ? &owner_ : nullptr;
}

bool PrincipalRegistry::binding_matches() const {
    return !has_binding_ || (has_owner_ && owner_.principal_id == binding_.principal_id &&
                             owner_.binding_revision == binding_.binding_revision);
}

bool PrincipalRegistry::bind(foundation::TransitionStamp stamp) {
    return has_owner_ && restore_binding({owner_.principal_id, owner_.binding_revision}, stamp);
}

bool PrincipalRegistry::restore_binding(const OwnerBindingRecord &record,
                                        foundation::TransitionStamp stamp) {
    if (has_binding_ || record.principal_id == 0U || record.binding_revision == 0U ||
        stamp.transition_id == 0U || stamp.commit_seq == 0U) {
        return false;
    }
    binding_ = {record.principal_id, record.binding_revision, stamp};
    has_binding_ = true;
    return true;
}

void PrincipalRegistry::drop_restored_binding() {
    binding_ = {};
    has_binding_ = false;
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
