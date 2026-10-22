// Finite field relationships only. A successful comparison is not a Grant
// check: subject, lifetime, revocation, Scene and budget belong to consumers.
#ifndef AYNI_CORE_AUTHORITY_SCOPE_SCOPE_H
#define AYNI_CORE_AUTHORITY_SCOPE_SCOPE_H

#include <ayni/authority.h>

#include <cstdint>

namespace ayni::authority {

struct Coverage {
    ayni_status_t status;
    bool covered; // meaningful only when status == AYNI_OK
};

// domain is a registry's domain_view. Output does not overlap the domain or
// typed inputs. Missing scope fields expand to the declared whole domain.
// BUFFER_TOO_SMALL writes only out_size; other failures write neither output.
ayni_status_t normalize_scope(ayni_bytes_v1 domain, const ayni_scope_v1 &scope, uint8_t *out,
                              uint32_t capacity, uint32_t &out_size);

// A malformed/out-of-domain operand is an error, distinct from two valid
// scopes whose containment is false. Each comparison considers one complete
// parent scope; it never combines independent Grants.
Coverage covers(ayni_bytes_v1 domain, const ayni_scope_v1 &parent, const ayni_scope_v1 &child);
Coverage contains_request(ayni_bytes_v1 domain, const ayni_scope_v1 &allowed,
                          const ayni_scope_v1 &actual);

// Canonical records are the durable form consumed by the next Grant package.
// Both shapes and containment in the current exact-version domain are checked.
Coverage covers(ayni_bytes_v1 domain, ayni_bytes_v1 parent, ayni_bytes_v1 child);
Coverage covers(ayni_bytes_v1 domain, ayni_bytes_v1 parent, const ayni_scope_v1 &child);
Coverage contains_request(ayni_bytes_v1 domain, ayni_bytes_v1 allowed, const ayni_scope_v1 &actual);

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_SCOPE_SCOPE_H
