// Accepted revocations survive replacement/reload of the Grant facts (C-AUTH-027).
#include "authority/grant/revocation_gate.h"
#include "foundation/kernel_id.h"
namespace ayni::authority {
using foundation::is_kernel_id;
bool RevocationGate::gated(uint64_t grant_id) const {
    for (uint32_t index = 0; index < ids_.size(); ++index) {
        if (ids_[index] == grant_id) {
            return true;
        }
    }
    return false;
}

bool RevocationGate::can_gate(uint64_t grant_id) const {
    return is_kernel_id(grant_id) && (gated(grant_id) || !ids_.full());
}

bool RevocationGate::install_gate(uint64_t grant_id) {
    return can_gate(grant_id) && (gated(grant_id) || ids_.push_back(grant_id));
}

bool RevocationGate::release_gate(uint64_t grant_id) {
    for (uint32_t index = 0; index < ids_.size(); ++index) {
        if (ids_[index] == grant_id) {
            ids_.remove_at(index);
            return true;
        }
    }
    return false;
}

} // namespace ayni::authority
