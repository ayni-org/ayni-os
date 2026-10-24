// ADR-0030/C-WIRE-017: raw authorization request identity, independent of
// current catalog, subject or parent availability. Omission stays omitted.
// Byte budgets: contracts/storage/records.md, raw create/revoke identity.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_CANONICAL_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_CANONICAL_H

#include <ayni/authority.h>

#include "kernel/command/command_approval.h"
#include "kernel/command/work_authority_result.h"

#include <cstdint>

namespace ayni::kernel {

constexpr uint32_t authority_command_format = 1;
constexpr uint32_t authority_command_fixed_max = 84;
constexpr uint32_t authority_command_max_bytes(uint32_t scope_max_bytes) {
    return authority_command_fixed_max + scope_max_bytes;
}

// Every exact command window uses one arena stride; the larger real payload
// owns its budget. Do not allocate approval parameters at the absolute ABI cap.
// requirements == 0 selects the input-only parser bound; configured windows
// supply their positive requirement capacity and include complete result bytes.
constexpr uint32_t exact_command_max_bytes(uint32_t scope_bytes, uint32_t parameter_bytes,
                                           uint32_t requirements = 0) {
    const auto approval = approval_command_max_bytes(parameter_bytes) +
                          (requirements == 0U ? 0U : work_authority_result_size(requirements));
    const auto grant = authority_command_max_bytes(scope_bytes);
    return grant > approval ? grant : approval;
}

// Revoke identity is kind/expected_revision (u32 each) plus target_id (u64),
// a separate fixed codec; create format 1 is unchanged.
constexpr uint32_t revoke_command_size = 16;
struct CanonicalRevokeView {
    uint32_t expected_revision;
    const ayni_grant_revoke_v1 *revoke;
};
struct RevokeCommandRecord {
    uint32_t expected_revision;
    uint64_t target_grant_id;
};
ayni_status_t encode_revoke_command(const CanonicalRevokeView &input, uint8_t *out,
                                    uint32_t capacity, uint32_t &out_size);
bool read_revoke_command(ayni_bytes_v1 bytes, RevokeCommandRecord &out);

struct CanonicalCommandView {
    uint32_t expected_revision;
    const ayni_grant_create_v1 *grant;
};

// Decoded views borrow the complete input bytes. No current registration is
// needed to validate this identity; scope remains the raw provided fields.
struct AuthorityCommandRecord {
    uint32_t expected_revision;
    uint32_t capability_version;
    ayni_string_v1 capability_id;
    uint64_t subject_instance_id;
    uint64_t parent_grant_id;
    ayni_grant_lifetime_t lifetime;
    uint32_t flags;
    uint64_t duration_us;
    ayni_bytes_v1 raw_scope;
};

// Validates complete bounded shape, not current authorization. Known Scene
// and quota flags remain representable; their semantic rejection follows
// command lookup. out_size changes only on success/BUFFER_TOO_SMALL.
ayni_status_t measure_authority_command(const CanonicalCommandView &input, uint32_t &out_size);
// Output must not overlap input views. No bytes change on failure.
ayni_status_t encode_authority_command(const CanonicalCommandView &input, uint8_t *out,
                                       uint32_t capacity, uint32_t &out_size);
// Strict format-1 parsing: sorted distinct fields/tokens and no trailing
// bytes. Publishes out only on success. Does not check catalog or existence.
bool read_authority_command(ayni_bytes_v1 bytes, AuthorityCommandRecord &out);
// Proves that the raw request, expanded against this source domain, is
// exactly the stored canonical scope. Streaming bounded views, no byte scratch.
bool authority_command_scope_matches(const AuthorityCommandRecord &command,
                                     ayni_bytes_v1 source_domain, ayni_bytes_v1 expanded_scope);
uint64_t authority_command_digest(ayni_bytes_v1 bytes);

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_CANONICAL_H
