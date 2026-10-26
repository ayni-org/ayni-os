// Grant facts, attenuation, expiry, revocation and safe replacement (ADR-0030/0031).
// Kernel resolves sessions/subjects and commits a checked placement; this
// owner never calls Host, Kernel, work or caller-provided code.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H

#include "authority/grant/grant_rules.h"
#include "authority/grant/revocation_gate.h"
#include "catalog/capability/capability_registry.h"
#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"

#include <ayni/authority.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::authority {

// contracts/storage/records.md, Grant lifecycle: one transition from ACTIVE
// to EXPIRED, REVOKED or GOAL_ENDED. Cleanup advances its own stamp, never the revision.
constexpr uint32_t grant_active_revision = 1;
constexpr uint32_t grant_terminal_revision = 2;
// Each retained Grant owns one full source domain and one expanded scope.
constexpr uint32_t grant_value_regions = 2;

// C-AUTH-026: a nonzero Grant names itself only for explicit root revocation.
// Raw metadata and retained facts share this identity rule; callers check state.
inline bool is_revocation_root(uint64_t grant_id, uint64_t revoked_by_root_id) {
    return grant_id == revoked_by_root_id;
}

// Caller-owned exclusions for one unchanged admission. Work derives current
// references; a batch planner may also exclude slots it has already selected.
// Indices belong to this GrantTable, never to persisted IDs (ADR-0037).
class GrantReclaimExclusions {
  public:
    // Only valid Grant indices enter; all bits initially permit reclamation.
    void add(uint32_t index) { words_[index / word_bits] |= uint64_t{1} << (index % word_bits); }
    bool contains(uint32_t index) const {
        return (words_[index / word_bits] & (uint64_t{1} << (index % word_bits))) != 0U;
    }

  private:
    static constexpr uint32_t word_bits = 64; // Bits in each uint64_t word, not an ID layout.
    static constexpr uint32_t word_count = (AYNI_GRANT_CAPACITY_MAX + word_bits - 1U) / word_bits;
    std::array<uint64_t, word_count> words_{}; // Fixed 128-byte mask; no allocation or live cache.
};

// Borrowed creation facts. The caller owns all views until admission/install
// finishes; record decoding supplies canonical scope separately from this view.
struct NewGrant {
    uint64_t subject_instance_id;    // Nonzero work instance ID; Kernel checks availability.
    uint64_t parent_grant_id;        // 0 for a root; otherwise one complete covering parent.
    uint64_t owner_principal_id;     // Nonzero authenticated owner captured for durable identity.
    uint32_t owner_binding_revision; // Nonzero owner identity revision, not a live registry mirror.
    catalog::CatalogId capability_id; // Exact registered ID, never an empty accepted fact.
    uint32_t capability_version;      // Exact nonzero schema version paired with source_domain.
    ayni_bytes_v1 source_domain;      // Borrowed complete canonical domain, at least its header.
    const ayni_scope_v1 *scope; // Typed constraint; null for canonical admission/decoded records.
    ayni_grant_lifetime_t lifetime; // PERMANENT or CURRENT_BOOT; determines duration semantics.
    uint64_t duration_us;           // Microseconds; 0 iff PERMANENT, positive for CURRENT_BOOT.
    uint32_t flags;                 // Known request bits; accepted facts use supported bits only.
    uint64_t bound_goal_id = 0; // 0 for generic authority; otherwise exact nondelegable root owner.
};

// Current owner-thread observations; none is serialized as a new Grant field.
struct GrantContext {
    uint64_t boot_epoch;  // Current nonzero durable boot epoch embedded in Kernel IDs.
    uint64_t now_us;      // Current boot's monotonic microseconds, never a cross-boot timestamp.
    uint64_t durable_seq; // Confirmed commit frontier; 0 means no confirmed Grant facts.
    uint64_t goal_id = 0; // Exact consuming Goal; 0 admits generic Grants only.
};

// Classified authority admission result, with no mutation on a refusal.
struct GrantCheck {
    ayni_status_t status; // AYNI_OK means all requested checks succeeded.
    ayni_reason_t reason; // NONE iff OK; otherwise the cause belonging to status.
};

// Retained immutable authority plus monotone terminal/cleanup state. IDs,
// owner, schema and duration are durable snapshots; volatile holds/deadlines
// never overwrite those facts (C-AUTH-020/021). Byte regions belong to the table.
struct Grant {
    uint64_t grant_id;                // Nonzero Kernel ID; creation boot is encoded in it.
    uint64_t subject_instance_id;     // Nonzero subject ID, checked by the work owner.
    uint64_t parent_grant_id;         // 0 for a root; one retained parent otherwise.
    uint64_t replaced_grant_id;       // 0 for append; exact evicted predecessor for replay.
    uint64_t owner_principal_id;      // Nonzero owner identity captured at creation.
    uint32_t owner_binding_revision;  // Nonzero revision paired with that captured owner.
    catalog::CatalogId capability_id; // Exact nonempty catalog identity for both byte regions.
    uint32_t capability_version;      // Nonzero schema version; no implicit upgrades.
    ayni_grant_lifetime_t lifetime;   // PERMANENT or CURRENT_BOOT, immutable after creation.
    uint64_t duration_us;  // Persisted us; 0 iff PERMANENT, never reanchored on recovery.
    bool allow_delegation; // Whether a narrower child may use this as its sole parent.
    uint32_t depth;        // Parent edges: root 0, otherwise parent.depth + 1.
    foundation::TransitionStamp created_stamp; // Original nonzero transition/commit identity.
    foundation::TransitionStamp stamp; // Creation or first terminal pair; cleanup cannot change it.
    // Retained facts use grant_active_revision iff ACTIVE; all terminal
    // facts use grant_terminal_revision, even while cleanup is incomplete.
    uint32_t revision;
    ayni_grant_lifecycle_t lifecycle; // ACTIVE -> EXPIRED/REVOKED once; never a compatibility hold.
    uint32_t source_domain_size; // Bytes in the first region, including the complete domain header.
    uint32_t scope_size;         // Bytes in the second region, with every omitted field expanded.
    // Current-boot monotonic us only. no_grant_deadline_us means PERMANENT or
    // a recovered finite fact that is unusable even before expiry is recorded.
    uint64_t deadline_us;
    ayni_reason_t hold_reason; // Volatile compatibility/subject cause; NONE when no hold exists.
    uint64_t revoked_by_root_id = 0; // 0 unless REVOKED; self for root, ancestor for a descendant.
    uint64_t cleanup_after_grant_id = 0; // Root's inclusive ID cursor; 0 before scanning anything.
    uint64_t cleanup_through_grant_id = 0; // Root's frozen inclusive frontier; 0 for non-roots.
    foundation::TransitionStamp cleanup_stamp{}; // Root progress pair; zero for non-roots.
    // Volatile lower bound on reclamation after a work reference was removed.
    // 0 means none. Reload drops/rebuilds rows; only the current writer frontier
    // may discharge it. It is not part of Grant records (ADR-0037).
    uint64_t reference_release_seq = 0;
    uint64_t bound_goal_id = 0; // Immutable Goal ID; 0 is generic, nonzero forbids delegation.
};

// Persistent terminal classification, independent of current time or catalog hold.
bool terminal(const Grant &grant);

struct GrantRecord;
struct GrantStateRecord;
struct GrantSnapshotRecord;
struct GrantRevocationRecord;
struct GrantRevocationProgressRecord;

// Owns bounded Grant rows, paired byte regions and volatile deny Gates. Work
// supplies subject/reference facts at Kernel boundaries; this owner has no I/O.
class GrantTable {
  public:
    // Fixed arena/count limits chosen before Kernel creation.
    struct Limits {
        uint32_t capacity;             // Number of retained rows and separately bounded Gates.
        uint32_t scope_max_bytes;      // Bytes in each domain/scope region; never a shared total.
        uint32_t delegation_depth_max; // Maximum parent edges; root depth is 0.
    };
    // Alignment and complete caller arena size, including Gates and both byte regions.
    static constexpr std::size_t storage_alignment = alignof(Grant);
    static constexpr std::size_t storage_size(Limits limits) {
        return foundation::BoundedArray<Grant>::storage_size(limits.capacity) +
               RevocationGate::storage_size(limits.capacity) +
               static_cast<std::size_t>(limits.capacity) * limits.scope_max_bytes *
                   grant_value_regions;
    }
    // Borrows aligned storage_size(limits) bytes for this table's entire lifetime.
    GrantTable(void *storage, Limits limits);
    // Retained row count, including terminal and held facts.
    uint32_t size() const { return grants_.size(); }
    // False for 0/missing IDs; failure preserves out_index.
    bool find(uint64_t grant_id, uint32_t &out_index) const;
    // Read-only retained fact; index must be below size().
    const Grant &at(uint32_t index) const { return grants_[index]; }
    // Borrowed canonical views at a valid index, invalidated by clear/replacement.
    ayni_bytes_v1 source_domain(uint32_t index) const;
    // Expanded constraint; same lifetime and index precondition as source_domain.
    ayni_bytes_v1 scope(uint32_t index) const;

    // Local checked path, leaf first. check_chain publishes 1..max+1 indices
    // only on success; an empty path in root Admission means no parent chain.
    // Indices borrow this unchanged table until the immediate caller finishes.
    struct CheckedChain {
        std::array<uint32_t, AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX + 1U>
            indices{};      // Valid prefix below count.
        uint32_t count = 0; // Number of valid indices; no persistent/cache state.
    };

    // A plan from typed or canonical admission. Batch tail indices are based
    // on the original size and may exceed it until earlier plans install.
    // Consume in order without unrelated mutation/callbacks; views remain
    // stable and do not alias any selected Grant row.
    struct Admission {
        uint32_t index;      // Reserved tail row or exact replacement; install in plan order.
        uint32_t depth;      // Parent edges of the new Grant, bounded by limits.
        uint32_t scope_size; // Complete normalized scope bytes, before writing any region.
        uint64_t replaced_grant_id; // 0 iff append; exact retained ID at index otherwise.
        uint64_t deadline_us; // Current-boot us; no_grant_deadline_us for permanent authority.
        uint64_t boot_epoch;  // Nonzero epoch required in the newly allocated Grant ID.
        CheckedChain parent_chain{}; // Already checked ancestors, for caller-owned subject rules.
    };
    // Local reservations for an unchanged table. Callers discard this entire
    // plan on refusal; selected slots are not live allocations (ADR-0037).
    struct BatchPlacement {
        GrantReclaimExclusions excluded; // Work references plus already reserved replacements.
        uint32_t appended = 0;           // Reserved tail rows, starting at the unchanged size().
    };
    // Validate shape, parent/chain, limits and replacement without changing the table;
    // publish out only on success. Caller checks subject facts using parent_chain.
    GrantCheck check_create(const NewGrant &grant, uint32_t expected_parent_revision,
                            GrantContext context, Admission &out,
                            const GrantReclaimExclusions &excluded = {}) const;
    // Consume that unchanged plan/input with a fresh ID/stamp; copy the byte views.
    bool insert(const NewGrant &grant, const Admission &admission, uint64_t new_grant_id,
                foundation::TransitionStamp stamp, uint32_t &out_index);
    // Admit a Goal-bound root using the work owner's immutable canonical
    // ceiling. No exporter or typed expansion; failed calls preserve plan/out.
    GrantCheck check_goal_create(const NewGrant &grant, ayni_bytes_v1 scope, GrantContext context,
                                 BatchPlacement &plan, Admission &out) const;
    // Consume one planned slot in admission order; borrowed bytes cannot alias
    // any selected Grant row and remain valid until the batch finishes.
    bool insert_goal(const NewGrant &grant, ayni_bytes_v1 scope, const Admission &admission,
                     uint64_t new_grant_id, foundation::TransitionStamp stamp, uint32_t &out_index);
    // Query-only chain admission when the caller needs no subject-path output.
    GrantCheck check_chain(uint32_t index, GrantContext context) const;
    // Same admission, publishing the checked path only on success. Does not
    // consult work subjects; Kernel consumes these indices without walking again.
    GrantCheck check_chain(uint32_t index, GrantContext context, CheckedChain &out) const;

    // Whether any retained row has a static boot catalog/subject hold.
    // A false sample skips per-reference walks only within the same owner query.
    bool has_compatibility_holds() const;
    // Whether this retained path has a boot catalog/subject compatibility hold.
    // This is availability only, never authorization; missing rows/links still
    // reach check_chain. No clock, Gate or lifecycle result is cached here.
    bool has_compatibility_hold(uint32_t index) const;

    // Preserve an old reference until its unlink is durable. Caller completed
    // all update checks; missing retained history needs no fence. seq is nonzero.
    void release_reference(uint64_t grant_id, uint64_t commit_seq);
    // Durable historical facts only: the retained chain was created before
    // commit_seq and any terminal transition came later. No current-clock test.
    bool active_at(uint32_t index, uint64_t commit_seq) const;
    // Immutable schema/Scope relation only; caller separately checks subject,
    // chosen revision and current chain usability. No catalog reinterpretation.
    bool covers_saved_scope(uint32_t index, const catalog::CatalogId &capability, uint32_t version,
                            ayni_bytes_v1 original_domain, ayni_bytes_v1 requested_scope) const;

    // Gates are accepted control state, independent of restored Grant slots.
    // Publishing pending+gate and their durable handoff is Kernel-owned.
    bool can_gate(uint64_t grant_id) const {
        return gates_.can_gate(grant_id);
    } // Valid ID and room, or an already installed Gate.
    bool install_gate(uint64_t grant_id) {
        return gates_.install_gate(grant_id);
    } // Idempotently deny this ID without changing Grant facts.
    bool release_gate(uint64_t grant_id) {
        return gates_.release_gate(grant_id);
    } // Remove exactly this ID; false when it has no Gate.
    bool gated(uint64_t grant_id) const {
        return gates_.gated(grant_id);
    } // Whether volatile accepted control denies this ID.
    uint32_t gate_count() const { return gates_.count(); } // Number of installed volatile denies.
    GrantCheck check_revoke(uint64_t grant_id,
                            uint32_t expected_revision) const; // First-accept CAS.
    // Result chosen from current facts for an already accepted control (C-AUTH-027).
    struct RevokePlan {
        uint64_t grant_id;           // Nonzero accepted target, even when the row has disappeared.
        uint32_t revision;           // 0 iff TARGET_GONE; terminal revision otherwise.
        ayni_intent_effect_t effect; // CHANGED, NO_CHANGE or TARGET_GONE for current facts.
        uint64_t cleanup_after_grant_id;   // 0 initially; zero for effects without new root state.
        uint64_t cleanup_through_grant_id; // Frozen retained-ID frontier only for CHANGED.
    };
    // Only for an already accepted revoke; it deliberately does not recheck
    // the original revision against facts changed by expiry or reload.
    RevokePlan plan_accepted_revoke(uint64_t grant_id) const;
    // Validate a live/replayed plan against current facts before any mutation.
    bool apply_revoke(const RevokePlan &plan, foundation::TransitionStamp stamp);
    // One bounded cursor advance, optionally revoking one active descendant.
    struct CleanupPlan {
        uint64_t root_grant_id;    // Nonzero explicit root whose cleanup is unfinished.
        uint64_t after_grant_id;   // Inclusive next ID/final watermark; advances the root cursor.
        uint64_t through_grant_id; // Same frozen frontier as the root; never extended by cleanup.
        uint64_t revoked_grant_id; // zero for a skipped candidate/final watermark
    };
    bool next_revocation_cleanup(CleanupPlan &out) const; // Oldest root first; false preserves out.
    bool apply_cleanup(const CleanupPlan &plan,
                       foundation::TransitionStamp stamp); // Atomic row/cursor change.
    // Least retained bound Goal ID in (after, through], or 0; includes orphaned
    // terminal Goal identities whose table slot has since been replaced.
    uint64_t next_bound_goal_after(uint64_t after, uint64_t through) const;
    // True only when this exact Goal still owns an ACTIVE dedicated root.
    bool has_active_goal_grants(uint64_t goal_id) const;
    // Caller proved this Goal terminal/absent. End its active dedicated roots
    // without a fabricated management revoke; retain original terminal order.
    void end_goal_grants(uint64_t goal_id, foundation::TransitionStamp stamp);

    // True when an ACTIVE finite Grant has a scheduled expiry, including a
    // due/old-boot expiry. The selected earliest deadline may still be future.
    bool next_expiry(GrantContext context, uint32_t &out_index, uint64_t &out_deadline) const;
    void expire(uint32_t index,
                foundation::TransitionStamp stamp); // Transition an active finite row.
    bool
    has_old_boot_expiry(uint64_t boot_epoch) const; // Includes restored rows without a deadline.
    void expire_old_boot(uint64_t boot_epoch,
                         foundation::TransitionStamp stamp); // One startup transition.
    void resolve_catalog(
        const catalog::CapabilityRegistry &catalog); // Recompute only volatile catalog holds.
    void hold_restored(uint32_t index,
                       ayni_reason_t reason); // Preserve the first volatile hold cause.

    // Record restore outcome; rejected records never partially publish a row.
    enum class Restore { restored, corrupt, no_room };
    // Replay a creation into its recorded append/replacement slot.
    Restore restore_create(const GrantRecord &record, foundation::TransitionStamp stamp,
                           const GrantReclaimExclusions &excluded = {});
    // Replay the one terminal transition, checking parent/root ordering.
    Restore restore_state(const GrantStateRecord &record, foundation::TransitionStamp stamp);
    // Append one retained fact after its parent, accepting later terminal ancestors.
    Restore restore_snapshot(const GrantSnapshotRecord &record);
    // Replay accepted root control; never recheck the original session/CAS.
    Restore restore_revocation(const GrantRevocationRecord &record,
                               foundation::TransitionStamp stamp);
    // Replay an ordered cursor after its optional same-batch descendant state.
    Restore restore_progress(const GrantRevocationProgressRecord &record,
                             foundation::TransitionStamp stamp);
    bool consistent() const; // Verify all retained parent/replacement/revocation relationships.
    void clear_restored() { grants_.clear(); } // Drop recovered facts while keeping accepted Gates.
    void clear() { // Full owner teardown: facts and accepted volatile controls are both discarded.
        clear_restored();
        gates_.clear();
    }

  private:
    friend class GrantRestore; // Same module facet: sole historical validator/publisher.
    enum class Walk { root, stopped, broken };
    // The starting index is known to exist. Budgets count visited nodes, not
    // edges: chain/history use max depth + root; descendant/frontier checks
    // use max depth. History keeps its extra node so a valid chain under a
    // smaller new limit reaches no_room, not corrupt (ADR-0030 decision 8).
    // Visitors are private owner predicates, never Host code.
    template <typename Visit>
    Walk visit_ancestors(uint32_t index, uint32_t node_budget, Visit visit) const {
        for (uint32_t visited = 0; visited < node_budget; ++visited) {
            if (!visit(index)) {
                return Walk::stopped;
            }
            const uint64_t parent = at(index).parent_grant_id;
            if (parent == 0U) {
                return Walk::root;
            }
            if (!find(parent, index)) {
                return Walk::broken;
            }
        }
        return Walk::broken;
    }
    bool has_child(uint64_t grant_id) const;
    bool next_id_after(uint64_t after_id, uint64_t through_id, uint32_t &out_index) const;
    bool descendant_of(uint32_t index, uint64_t root_id) const;
    bool reclaimable(uint32_t index, uint64_t durable_seq) const;
    GrantCheck reserve_placement(uint64_t durable_seq, BatchPlacement &plan,
                                 Admission &admission) const;
    bool valid_install(const Admission &admission, uint64_t id,
                       foundation::TransitionStamp stamp) const;
    void publish(const NewGrant &grant, const Admission &admission, uint64_t id,
                 foundation::TransitionStamp stamp, uint32_t &out_index);
    GrantCheck check_parent_contract(uint32_t parent_index, const NewGrant &child) const;
    bool valid_link(uint32_t child, uint32_t parent) const;
    uint8_t *domain_storage(uint32_t index) const;
    uint8_t *scope_storage(uint32_t index);
    foundation::BoundedArray<Grant> grants_;
    RevocationGate gates_; // Owns accepted volatile denies; clear_restored never clears it.
    uint8_t *bytes_;
    Limits limits_;
};

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_GRANT_GRANT_TABLE_H
