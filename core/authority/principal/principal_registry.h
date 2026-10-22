// Principals and their sessions (contracts/domain/primitives.md, C-AUTH-010).
// The host authenticates both: it registers the owner it has bound and opens
// a session for each channel it has verified. An intent acts for its
// session's principal and channel and can claim no other. Sessions are not
// stored; a reboot closes them all.

#ifndef AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H
#define AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H

#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::authority {

struct Principal {
    uint64_t principal_id;
    uint32_t binding_revision;
    ayni_principal_kind_t kind;
};

struct OwnerBinding {
    uint64_t principal_id;
    uint32_t binding_revision;
    foundation::TransitionStamp stamp;
};

struct OwnerBindingRecord;

// v0.1 has exactly one principal, the owner (C-PROD-001).
class PrincipalRegistry {
  public:
    static constexpr uint32_t capacity = 1; // the owner
    // Returns false, changing nothing, for an invalid owner or when one is
    // already registered. Only OWNER with a nonzero id and revision is valid.
    bool add_owner(const Principal &principal);

    const Principal *find(uint64_t principal_id) const;
    // The binding the Host expects this boot, independent of the binding
    // recovered from storage. Null when the Host registered no owner.
    const Principal *owner() const { return has_owner_ ? &owner_ : nullptr; }

    bool binding_known() const { return has_binding_; }
    const OwnerBinding *binding() const { return has_binding_ ? &binding_ : nullptr; }
    // An unbound store accepts its first owner. A known binding requires
    // that the Host registered exactly that principal and binding revision.
    bool binding_matches() const;
    bool needs_binding() const { return has_owner_ && !has_binding_; }
    // Establishes the Host's expected binding in a new transition. Returns
    // false without a change when already bound, no owner is registered or
    // either part of the stamp is zero.
    bool bind(foundation::TransitionStamp stamp);
    // A binding record occurs once. The record must have a nonzero principal
    // and binding revision, and stamp must be nonzero. A Host mismatch does
    // not make the record corrupt; binding_matches() reports it separately.
    bool restore_binding(const OwnerBindingRecord &record, foundation::TransitionStamp stamp);
    // Reload clears recovered facts, retaining the Host's expected owner.
    void drop_restored_binding();

  private:
    Principal owner_{};
    OwnerBinding binding_{};
    bool has_owner_ = false;
    bool has_binding_ = false;
};

struct Session {
    uint64_t session_id;
    uint64_t principal_id;
    ayni_channel_t channel;
    uint32_t binding_revision;
};

class SessionTable {
  public:
    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Session>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Session>::storage_size(capacity);
    }

    // storage as for BoundedArray<Session>.
    SessionTable(void *storage, uint32_t capacity) : sessions_(storage, capacity) {}

    bool full() const { return sessions_.full(); }

    // session has an id that no open session has. Returns false, changing
    // nothing, when full.
    bool open(const Session &session) { return sessions_.push_back(session); }

    // Returns false when no open session has the id.
    bool close(uint64_t session_id);
    void clear() { sessions_.clear(); }

    const Session *find(uint64_t session_id) const;

  private:
    foundation::BoundedArray<Session> sessions_;
};

} // namespace ayni::authority

#endif // AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H
