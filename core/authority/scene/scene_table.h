// Scene declarations and durable owner preference. No I/O, observations or permits are owned here.
#ifndef AYNI_CORE_AUTHORITY_SCENE_TABLE_H
#define AYNI_CORE_AUTHORITY_SCENE_TABLE_H
#include "catalog/capability/capability_registry.h"
#include "catalog/observation/source_registry.h"
#include "foundation/transition_stamp.h"
#include <array>
#include <ayni/scenes.h>

namespace ayni::authority {
// Explicit persistent owner preference; zero is not a record state.
enum class SceneMode : uint32_t { disabled = 1, enabled = 2 };

// Pointer-free owner command semantics; authentication/session stay outside retry identity.
struct SceneChange {
    catalog::CatalogId scene;    // Exact catalog identity, never an untrusted path.
    uint32_t definition_version; // Positive current version for enable; zero for disable.
    uint32_t expected_revision;  // Zero for first state, otherwise exact persistent revision.
    SceneMode mode;              // Desired owner preference, not effective authorization.
};
// Shared semantic validator for typed commands and decoded command records.
ayni_status_t validate_scene_change(const SceneChange &change);

// Durable row; original declaration bytes belong to the matching fixed table slot.
struct SceneState {
    uint64_t object_id;                // Stable nonzero Kernel ID, never a catalog ordinal.
    catalog::CatalogId scene;          // Immutable identity of this retained owner preference.
    uint32_t definition_version;       // Positive version of the saved complete declaration.
    uint32_t revision;                 // Positive CAS revision, never wraps.
    SceneMode mode;                    // Saved preference; compatibility/durability may still deny.
    uint32_t definition_size;          // Valid byte prefix of the fixed original-definition slot.
    foundation::TransitionStamp stamp; // Last state change, governs enable publication.
};

// One source prerequisite plus its original trusted type, retained for config compatibility.
struct SceneCondition {
    catalog::ObservationCondition predicate; // Finite source/version/range/age requirement.
    catalog::ObservationSource source;       // Original kind/unit/origin/validity contract.
};
// Metadata points only into its owner's definition byte slot, never into caller input.
struct SceneRule {
    catalog::CatalogId capability; // Nonempty exact capability identity.
    uint32_t version;              // Positive exact semantics version.
    uint32_t domain_offset;        // Byte offset inside normalized definition, no external pointer.
    uint32_t domain_size;          // Complete original authority domain length.
    uint32_t scope_offset;         // Byte offset of the normalized allowed Scope.
    uint32_t scope_size;           // Complete canonical Scope length.
};
// Immutable registered Scene metadata; unused array cells contain no semantic data.
struct SceneDefinition {
    catalog::CatalogId id;    // Unique nonempty configured name.
    uint32_t version;         // Positive configured semantics version.
    uint32_t bytes;           // Canonical definition byte length in its fixed arena stride.
    uint32_t rule_count;      // Valid rules prefix, 1..SCENE_RULE_COUNT_MAX.
    uint32_t condition_count; // Valid predicates prefix, 0..OBSERVATION_CONDITION_COUNT_MAX.
    std::array<SceneRule, AYNI_SCENE_RULE_COUNT_MAX> rules; // Sorted exact capability/version set.
    std::array<SceneCondition, AYNI_OBSERVATION_CONDITION_COUNT_MAX> conditions; // Unique sources.
};

// Admission result valid only until the owner applies another mutation.
struct PreparedSceneChange {
    uint32_t state_index;      // Existing state or no_index for first persisted preference.
    uint32_t definition_index; // Configured definition or no_index when disabling an orphan.
    uint32_t next_revision;    // Current revision for no change, otherwise current + 1.
    bool changes_state;        // False only for an already saved identical preference/definition.
};

// Owns startup descriptions and separate persisted preferences. No implicit eviction:
// removed/incompatible Scene preferences remain queryable until a future explicit migration.
class Scenes {
  public:
    struct Limits {
        uint32_t capacity;    // Bounds both configured and retained states; zero disables.
        uint32_t scope_bytes; // Existing authority domain/Scope bound, reused for each rule.
    };
    static constexpr uint32_t no_index = UINT32_MAX;
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static uint32_t definition_max_bytes(uint32_t scope_bytes); // Canonical worst-case declaration.
    static std::size_t storage_size(Limits limits); // Metadata, two fixed pools and staging space.
    // All caller storage and borrowed catalogs outlive this owner; no allocation.
    Scenes(void *storage, Limits limits, const catalog::CapabilityRegistry &capabilities,
           const catalog::ObservationSources &sources);
    // CONFIGURING caller policy; all descriptors/scopes copied before returning.
    ayni_status_t register_description(const ayni_scene_desc_v1 &input);
    // Independent named lookups; retained state can exist without a current description.
    bool find_definition(ayni_string_v1 id, uint32_t &index) const;
    bool find_state(ayni_string_v1 id, uint32_t &index) const;
    // Immutable owner metadata, indices less than the respective counts.
    const SceneDefinition &definition(uint32_t index) const { return definitions_[index]; }
    const SceneState &state(uint32_t index) const { return states_[index]; }
    uint32_t state_count() const { return states_.size(); } // Retained states, not declarations.
    void clear_states() { states_.clear(); } // Recovery discard preserves startup declarations.
    // Exact copied configured bytes; stable until destruction.
    ayni_bytes_v1 definition_view(uint32_t index) const;
    // Stored original bytes; view borrows this state slot until its next mutation.
    ayni_bytes_v1 state_definition(uint32_t index) const;
    // Argument validation happened once; CAS, configuration and capacity before IDs/stamps.
    ayni_status_t prepare(const SceneChange &change, PreparedSceneChange &out) const;
    // Prepared owner-thread mutation; new object ID only for first state, otherwise unchanged.
    void apply(const SceneChange &change, const PreparedSceneChange &prepared, uint64_t object_id,
               foundation::TransitionStamp stamp);
    // Exact enabled/configuration/durable gate, followed by this Scene's allowed capability scope.
    ayni_status_t authorize(ayni_string_v1 id, uint32_t version, ayni_string_v1 capability,
                            uint32_t capability_version, ayni_bytes_v1 arguments,
                            uint64_t durable_seq, uint32_t &definition_index) const;
    // Read-only metadata/state projection. Missing both descriptions/state returns NOT_FOUND.
    ayni_status_t info(ayni_string_v1 id, uint64_t durable_seq, ayni_scene_info_v1 &out) const;
    // Replay installs already decoded/validated complete state and original declaration.
    // Capacity/identity/revision checks preserve all slots on refusal.
    ayni_status_t restore_validated(const SceneState &state, ayni_bytes_v1 definition);

  private:
    // One byte-stride calculation for each owner pool; zero capacity never dereferences a slot.
    uint8_t *definition_bytes(uint32_t index) const;
    uint8_t *state_bytes(uint32_t index) const;
    bool matches(uint32_t definition_index, uint32_t state_index) const;
    Limits limits_;                                         // Immutable startup bounds.
    uint32_t definition_stride_;                            // Positive derived bound when enabled.
    const catalog::CapabilityRegistry &capabilities_;       // Trusted immutable authority domains.
    const catalog::ObservationSources &sources_;            // Trusted immutable observation types.
    foundation::BoundedArray<SceneDefinition> definitions_; // Static metadata owner.
    foundation::BoundedArray<SceneState> states_;           // Persistent preference owner.
    uint8_t *definitions_bytes_;                            // Canonical startup declaration bytes.
    uint8_t *states_bytes_; // Saved exact declarations, independent of current config.
    uint8_t *scratch_;      // One register-description staging area; never persisted directly.
};
} // namespace ayni::authority
#endif
