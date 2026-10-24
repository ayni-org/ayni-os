// Goal-owned authority ceilings and selected dependencies (ADR-0037).
// Presence is explicit: absent metadata never means a declared empty ceiling.
// This module owns saved facts, not catalog lookup, Grant selection or I/O.
#ifndef AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_H
#define AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_H

#include "catalog/catalog_id.h"
#include "foundation/transition_stamp.h"
#include "work/authority/authority_facts.h"

#include <ayni/authority.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::work {

// MA-006 implementation plan 6.1: requirements per Goal, independent of
// each region's existing authority_scope_max_bytes limit.
constexpr uint32_t goal_requirement_regions = 2; // Original domain and normalized scope.

// Persisted state of an explicit ceiling. Missing metadata is represented
// by no table entry, never by one of these states (records.md type 17/v2).
enum class GoalAuthorityPhase : uint32_t { proposed = 1, authorized = 2, paused = 3 };

// A selected immutable authority identity; a missing dependency is {0, 0}.
// Revision is the selected Grant revision, not a mirror of its current state.
using GoalGrantReference = AuthorityReference;

// Borrowed complete requirement. Byte views survive only while their source
// remains unchanged. Each requirement is covered by one Grant, never a union.
struct GoalRequirement {
    catalog::CatalogId capability_id; // Nonempty exact registered identity.
    uint32_t capability_version = 0;  // Nonzero exact version of source_domain.
    ayni_bytes_v1 source_domain{};    // Original canonical domain bytes; never empty.
    ayni_bytes_v1 scope{};            // Complete canonical requested ceiling within source_domain.
    GoalGrantReference dependency;    // Missing in a proposal, selected otherwise.
};

// Complete saved ceiling and references. Only requirements[0,count) exist;
// count == 0 explicitly declares no authority needs, unlike absent metadata.
struct GoalAuthorityRecord {
    uint64_t goal_id = 0;                // Nonzero Kernel ID of the owning Goal.
    foundation::TransitionStamp stamp{}; // Nonzero pair of the last authority change.
    GoalAuthorityPhase phase = GoalAuthorityPhase::authorized; // Decides dependency completeness.
    uint32_t count = 0;  // Requirements, at most AYNI_GOAL_REQUIREMENT_CAPACITY_MAX.
    GoalBinding binding; // None only for proposals; immutable between explicit admissions.
    std::array<GoalRequirement, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> requirements{};
};

// One semantic validator for borrowed struct facts and decoded bytes. Does
// not consult today's catalog or claim that any selected Grant is usable.
bool valid_goal_authority_record(const GoalAuthorityRecord &record);

// Original absent references, independent of the current validity of selected Grants.
uint32_t missing_requirement_mask(const GoalAuthorityRecord &record);

// Owns one independently bounded slot per GoalTable index. Replacement clears
// the old Goal's metadata; consumers also match goal_id so slot reuse cannot
// transfer authority. All methods run on the Kernel owner thread.
class GoalAuthorityTable {
  public:
    // Fixed caller-owned arena dimensions; each dimension is validated before construction.
    struct Limits {
        uint32_t goals;        // Positive Goal slots, matching the accompanying GoalTable capacity.
        uint32_t requirements; // Positive requirements per Goal, bounded by the maximum.
        uint32_t scope_bytes;  // Bytes per domain/scope, within the existing Scope limits.
    };

    // Whole arena alignment and size; validated limits keep 32-bit sizes bounded.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(Limits limits);
    // Borrows storage_size(limits) aligned bytes until destruction. No allocation.
    GoalAuthorityTable(void *storage, Limits limits);
    GoalAuthorityTable(const GoalAuthorityTable &) = delete;
    GoalAuthorityTable &operator=(const GoalAuthorityTable &) = delete;

    // Append one registered requirement into dedicated staging byte regions.
    // out starts empty; sources stay stable through this call. Refusal preserves
    // count and all prior entries. Staging views expire at the next export.
    ayni_status_t stage(GoalAuthorityRecord &out, const catalog::CatalogId &capability,
                        uint32_t version, ayni_bytes_v1 domain, const ayni_scope_v1 &scope);
    // Change only the authorization phase and stamp of a present complete row.
    // User pause and compatibility holds belong to their existing work owners.
    void pause(uint32_t goal_index, foundation::TransitionStamp stamp);
    // Publish one admitted activation/replacement binding, preserving its Scope
    // ceiling. Caller preflighted the whole batch; no callbacks may intervene.
    void bind(uint32_t goal_index,
              const std::array<GoalGrantReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> &references,
              foundation::TransitionStamp stamp, BindingReference parent = {},
              InputResumeBoundary input = {});
    // Missing metadata is blocked; callers distinguish legacy holds separately.
    bool authorized(uint32_t goal_index, uint64_t goal_id) const;

    // Requires facts accepted by valid_goal_authority_record or the record reader,
    // unchanged since admission. Checks configured capacity before copying;
    // sources must not alias live slots; dedicated staging regions are allowed.
    // Refusal preserves every live slot and byte.
    ayni_status_t install_validated(uint32_t goal_index, const GoalAuthorityRecord &record);
    // Replay a validated record. Existing ceilings are immutable and both stamp
    // components must advance; capacity and refusal guarantees match install.
    ayni_status_t restore_validated(uint32_t goal_index, const GoalAuthorityRecord &record);
    // Presence for this exact Goal, including its explicit zero-requirement row.
    bool contains(uint32_t goal_index, uint64_t goal_id) const;
    // Borrowed complete view at a known present slot. Caller keeps the table
    // unchanged, as during snapshot freeze; no repeated identity admission.
    GoalAuthorityRecord at(uint32_t goal_index) const;
    // Publishes borrowed views only for the exact owning Goal; failure preserves
    // out. Views are invalidated by install/clear at the same index or clear_all.
    bool read(uint32_t goal_index, uint64_t goal_id, GoalAuthorityRecord &out) const;
    // Discards metadata at a valid index; an out-of-range index changes nothing.
    void clear(uint32_t goal_index);
    // Recovery reset; caller already fenced all consumption.
    void clear_all();

  private:
    // Slot presence is explicit, independent of count (which may validly be 0).
    struct Slot {
        uint64_t goal_id = 0; // 0 only when !present; otherwise the owning Kernel ID.
        foundation::TransitionStamp stamp{}; // Last update, zero only when absent.
        GoalAuthorityPhase phase = GoalAuthorityPhase::authorized;
        uint32_t count = 0;   // Valid prefix in this slot's Requirement array.
        GoalBinding binding;  // Selected generation/parent/input frontier; no Grant state cache.
        bool present = false; // True even for an explicitly empty authorized ceiling.
    };
    // Owned metadata; its bytes live in the corresponding two fixed regions.
    struct Requirement {
        catalog::CatalogId capability_id;
        uint32_t capability_version = 0; // Exact nonzero version when slot is present.
        uint32_t domain_size = 0;        // Used bytes in domain, never above scope_bytes.
        uint32_t scope_size = 0;         // Used bytes in scope, never above scope_bytes.
        GoalGrantReference dependency;   // Selected identity or explicit missing pair.
    };
    // Flat index and regions share the same stride; only validated indices enter.
    uint32_t requirement_index(uint32_t goal_index, uint32_t requirement) const;
    uint8_t *domain_at(uint32_t index) const;
    uint8_t *scope_at(uint32_t index) const;
    // Compares an existing slot's immutable ceiling, excluding refs/phase/stamp.
    bool same_ceiling(uint32_t goal_index, const GoalAuthorityRecord &record) const;
    Slot *slots_;               // Caller arena; limits.goals constructed Slot objects.
    Requirement *requirements_; // Caller arena; goals * requirements constructed objects.
    uint8_t *bytes_;            // Paired byte regions after the metadata arrays.
    Limits limits_;             // Immutable arena bounds, not a copy of live Goal state.
};

} // namespace ayni::work
#endif // AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_H
