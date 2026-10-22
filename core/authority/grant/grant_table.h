// Grant facts, attenuation, expiry and safe leaf replacement (ADR-0030).
// Kernel resolves sessions/subjects and commits a checked placement; this
// owner never calls Host, Kernel, work or caller-provided code.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H

#include "catalog/capability/capability_registry.h"
#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"

#include <ayni/authority.h>

#include <cstddef>
#include <cstdint>

namespace ayni::authority {

struct NewGrant {
    uint64_t subject_instance_id;
    uint64_t parent_grant_id;
    uint64_t owner_principal_id;
    uint32_t owner_binding_revision;
    catalog::CatalogId capability_id;
    uint32_t capability_version;
    ayni_bytes_v1 source_domain;
    const ayni_scope_v1 *scope;
    ayni_grant_lifetime_t lifetime;
    uint64_t duration_us;
    uint32_t flags;
};

struct GrantContext {
    uint64_t boot_epoch;
    uint64_t now_us;
    uint64_t durable_seq;
};

struct GrantCheck {
    ayni_status_t status;
    ayni_reason_t reason;
};

struct Grant {
    uint64_t grant_id;
    uint64_t subject_instance_id;
    uint64_t parent_grant_id;
    uint64_t replaced_grant_id;
    uint64_t owner_principal_id;
    uint32_t owner_binding_revision;
    catalog::CatalogId capability_id;
    uint32_t capability_version;
    ayni_grant_lifetime_t lifetime;
    uint64_t duration_us;
    bool allow_delegation;
    uint32_t depth;
    foundation::TransitionStamp created_stamp;
    foundation::TransitionStamp stamp;
    uint32_t revision;
    ayni_grant_lifecycle_t lifecycle;
    uint32_t source_domain_size;
    uint32_t scope_size;
    uint64_t deadline_us;      // this boot only; never serialized or reanchored
    ayni_reason_t hold_reason; // this boot only; never serialized
};

struct GrantRecord;
struct GrantStateRecord;
struct GrantSnapshotRecord;

class GrantTable {
  public:
    struct Limits {
        uint32_t capacity;
        uint32_t scope_max_bytes;
        uint32_t delegation_depth_max;
    };
    static constexpr std::size_t storage_alignment = alignof(Grant);
    static constexpr std::size_t storage_size(Limits limits) {
        return foundation::BoundedArray<Grant>::storage_size(limits.capacity) +
               static_cast<std::size_t>(limits.capacity) * limits.scope_max_bytes * 2U;
    }
    GrantTable(void *storage, Limits limits);
    uint32_t size() const { return grants_.size(); }
    bool find(uint64_t grant_id, uint32_t &out_index) const;
    const Grant &at(uint32_t index) const { return grants_[index]; }
    ayni_bytes_v1 source_domain(uint32_t index) const;
    ayni_bytes_v1 scope(uint32_t index) const;

    // A plan from check_create, consumed without an intervening mutation or
    // callback. Inputs remain stable and do not alias the replacement slot.
    struct Admission {
        uint32_t index;
        uint32_t depth;
        uint32_t scope_size;
        uint64_t replaced_grant_id;
        uint64_t deadline_us;
        uint64_t boot_epoch;
    };
    GrantCheck check_create(const NewGrant &grant, uint32_t expected_parent_revision,
                            GrantContext context, Admission &out) const;
    bool insert(const NewGrant &grant, const Admission &admission, uint64_t new_grant_id,
                foundation::TransitionStamp stamp, uint32_t &out_index);
    GrantCheck check_chain(uint32_t index, GrantContext context) const;

    // True when an ACTIVE finite Grant has a scheduled expiry, including a
    // due/old-boot expiry. The selected earliest deadline may still be future.
    bool next_expiry(GrantContext context, uint32_t &out_index, uint64_t &out_deadline) const;
    void expire(uint32_t index, foundation::TransitionStamp stamp);
    bool has_old_boot_expiry(uint64_t boot_epoch) const;
    void expire_old_boot(uint64_t boot_epoch, foundation::TransitionStamp stamp);
    void resolve_catalog(const catalog::CapabilityRegistry &catalog);
    void hold_restored(uint32_t index, ayni_reason_t reason);

    enum class Restore { restored, corrupt, no_room };
    Restore restore_create(const GrantRecord &record, foundation::TransitionStamp stamp);
    Restore restore_state(const GrantStateRecord &record, foundation::TransitionStamp stamp);
    Restore restore_snapshot(const GrantSnapshotRecord &record);
    bool consistent() const;
    void clear() { grants_.clear(); }

  private:
    bool has_child(uint64_t grant_id) const;
    bool same_contract(uint32_t left, const NewGrant &right) const;
    bool valid_link(uint32_t child, uint32_t parent) const;
    Restore restore(const GrantRecord &record, foundation::TransitionStamp created,
                    foundation::TransitionStamp stamp, uint32_t revision,
                    ayni_grant_lifecycle_t lifecycle, bool snapshot);
    uint8_t *domain_storage(uint32_t index);
    uint8_t *scope_storage(uint32_t index);
    foundation::BoundedArray<Grant> grants_;
    uint8_t *bytes_;
    Limits limits_;
};

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H
