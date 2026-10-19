// Principals and their sessions (contracts/domain/primitives.md, C-AUTH-010).
// The host authenticates both: it registers the owner it has bound and opens
// a session for each channel it has verified. An intent acts for its
// session's principal and channel and can claim no other. Sessions are not
// stored; a reboot closes them all.

#ifndef AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H
#define AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H

#include "foundation/bounded_array.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::authority {

struct Principal {
    uint64_t principal_id;
    uint32_t binding_revision;
    ayni_principal_kind_t kind;
};

// v0.1 has exactly one principal, the owner (C-PROD-001).
class PrincipalRegistry {
  public:
    // principal is an owner with a nonzero id and binding revision. Returns
    // false, changing nothing, when an owner is already registered.
    bool add_owner(const Principal &principal);

    const Principal *find(uint64_t principal_id) const;

  private:
    Principal owner_{};
    bool has_owner_ = false;
};

struct Session {
    uint64_t session_id;
    uint64_t principal_id;
    ayni_channel_t channel;
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

    const Session *find(uint64_t session_id) const;

  private:
    foundation::BoundedArray<Session> sessions_;
};

} // namespace ayni::authority

#endif // AYNI_CORE_AUTHORITY_PRINCIPAL_PRINCIPAL_REGISTRY_H
