// Bounded memory facts and provenance. Authorization and work orchestration stay in Kernel.
#ifndef AYNI_CORE_KNOWLEDGE_MEMORY_TYPES_H
#define AYNI_CORE_KNOWLEDGE_MEMORY_TYPES_H
#include "foundation/transition_stamp.h"
#include <array>
#include <ayni/memory.h>
#include <cstdint>
namespace ayni::knowledge {

// M1 fixed maxima bound all local traversals and candidate buffers (ADR-0060).
constexpr uint32_t memory_capacity_max = AYNI_MEMORY_CAPACITY_MAX;
constexpr uint32_t memory_body_max = AYNI_MEMORY_BODY_BYTES_MAX;
constexpr uint32_t memory_dependency_max = AYNI_MEMORY_DEPENDENCY_CAPACITY_MAX;
constexpr uint32_t memory_tag_max = AYNI_MEMORY_TAGS_MAX;
constexpr uint32_t memory_keyword_max = AYNI_MEMORY_KEYWORDS_MAX;
constexpr uint32_t memory_initial_revision = 1;
constexpr uint32_t memory_label_max_bytes = AYNI_MEMORY_LABEL_BYTES_MAX;

// Immutable table configuration, with fixed memory chosen before Kernel creation.
struct MemoryLimits {
    uint32_t capacity;     // 0 disables; otherwise 1..memory_capacity_max retained records.
    uint32_t body_bytes;   // Positive UTF-8 byte stride when enabled, at most memory_body_max.
    uint32_t dependencies; // 1..memory_dependency_max direct sources per record when enabled.
};

// A private namespace belongs to one retained Agent instance; profile belongs to owner.
enum class MemoryNamespace : uint32_t { private_agent = 1, owner_profile = 2 };
// Explicit user content and confirmation are separate facts; models cannot claim user origin.
enum class MemorySource : uint32_t { user = 1, model = 2, derived = 3 };
// Profile fields are the explicitly saved M1 basics, not arbitrary inferred facts.
enum class ProfileField : uint32_t { none = 0, name = 1, language = 2, interaction = 3 };
// Tombstone forbids use before bounded scrubbing; scrubbed retains only audit identity.
enum class MemoryState : uint32_t { live = 1, tombstone = 2, scrubbed = 3 };
// Anchor is a conversation invalidation barrier, never an implicit summary of memory lineage.
enum class DependencyKind : uint32_t { memory = 1, context = 2 };
// Stable purposes supported by M1 capability declarations.
enum class MemoryPurpose : uint32_t { context = 1, organize = 2 };

// Exact authorization captured at a read/export; zero pair means implicit private ownership.
struct MemoryGrantRef {
    uint64_t id = 0;       // Kernel Grant ID; zero iff revision is zero.
    uint32_t revision = 0; // Exact captured Grant revision; no replacement selection on use.
};

// Direct source plus the actual permissions used. Transitive sources remain in memory records.
struct MemoryDependency {
    DependencyKind kind = DependencyKind::memory; // Selects record or context-anchor owner.
    uint64_t id = 0;                              // Nonzero source ID, never a moving slot.
    uint32_t revision = 0;                        // Exact source version, positive.
    uint64_t actor_instance_id =
        0; // Actual reading/exporting Agent; implicit private access is tied to it.
    MemoryPurpose purpose = MemoryPurpose::context; // Actual authorized usage, not free text.
    MemoryGrantRef read{}; // Exact shared/profile read Grant; empty for private or context source.
    MemoryGrantRef egress{}; // Exact export Grant when body entered a model; empty before export.
};

// A complete bounded set; count zero means independent explicit input, never truncation.
struct MemoryDependencies {
    std::array<MemoryDependency, memory_dependency_max> items{}; // Valid entries below count.
    uint32_t count = 0; // Bounded by the configured dependency limit, including context anchors.
};

// Owns one nonempty, bounded UTF-8 search label; unused bytes remain zero.
class MemoryLabel {
  public:
    // Failure leaves the previous label unchanged; input may alias this label.
    bool assign(ayni_string_v1 text);
    // Exact UTF-8 byte equality, with no case folding or locale policy.
    bool equals(ayni_string_v1 text) const;
    // Borrowed bytes, valid through the next assignment; no trailing terminator required.
    const char *data() const { return bytes_.data(); }
    // UTF-8 byte count, zero only for an unused label.
    uint32_t size() const { return size_; }

  private:
    std::array<char, memory_label_max_bytes> bytes_{}; // Owned content; zero suffix.
    uint32_t size_ = 0; // Active prefix, at most memory_label_max_bytes.
};

// Exact tags/keywords; body search uses exact UTF-8 byte substring.
struct MemoryLabels {
    std::array<MemoryLabel, memory_tag_max> tags{};         // Unique active prefix below tag_count.
    std::array<MemoryLabel, memory_keyword_max> keywords{}; // Unique active keyword prefix.
    uint32_t tag_count = 0;                                 // Zero means no tags.
    uint32_t keyword_count = 0;                             // Zero means no keywords.
};

// Retained facts; the table separately owns body bytes at its row index.
struct MemoryRecord {
    uint64_t id = 0;       // Nonzero global Kernel ID, never reused after removal.
    uint32_t revision = 0; // Positive CAS revision; no wrap on edit/confirm/delete.
    MemoryNamespace space = MemoryNamespace::private_agent; // Owner kind for namespace_id.
    uint64_t namespace_id = 0; // Agent instance or bound owner principal; never zero.
    MemorySource source =
        MemorySource::user; // Original semantic source, unchanged by confirmation.
    ProfileField profile_field = ProfileField::none; // Non-none only for owner_profile basics.
    bool confirmed = false;                          // Only a management Session can set this flag.
    MemoryState state = MemoryState::live; // Monotone live -> tombstone -> scrubbed on deletion.
    uint32_t body_size = 0;                // UTF-8 bytes; positive while live, zero once scrubbed.
    uint64_t source_call_id =
        0; // Optional model-call audit identity; zero for no originating call.
    MemoryLabels
        labels{}; // Search content is scrubbed along with body, never retained in tombstone audit.
    MemoryDependencies dependencies{}; // Never cleared by confirmation; invalid sources forbid use.
    foundation::TransitionStamp
        stamp{}; // Last accepted mutation; positive transition and sequence.
};

// Borrowed candidate; callers retain all views unchanged until the table operation completes.
struct MemoryValue {
    MemoryNamespace space; // Requested namespace kind, cannot change on an existing record.
    uint64_t namespace_id; // Nonzero namespace owner, authenticated by Kernel.
    MemorySource source;   // Set by the management/policy admission path, not inferred from body.
    ProfileField profile_field; // M1 profile field or none for private memories.
    bool confirmed;             // Set by trusted admission only, not a model-controlled flag.
    uint64_t source_call_id;    // Optional originating call audit ID.
    ayni_string_v1 body; // Nonempty UTF-8, length limited by configuration; no terminator needed.
    MemoryLabels labels; // Complete bounded searchable labels.
    MemoryDependencies dependencies; // Complete accumulated source set, never truncated.
};

// Shared intrinsic validation for typed proposals and decoded values; bounds precede byte reads.
ayni_status_t check_memory_value(const MemoryValue &value, MemoryLimits limits);
// Namespace and profile field form, independent of authentication/existence.
bool valid_memory_namespace(MemoryNamespace space, uint64_t id, ProfileField field);
// Known original source classification, independent of confirmation.
bool valid_memory_source(MemorySource source);
// Borrow the value portion of retained metadata without losing source or lineage.
MemoryValue memory_value(const MemoryRecord &record, ayni_string_v1 body);

// Exact semantic equality, including labels, trust and the complete dependency set.
bool same_memory_value(const MemoryValue &left, const MemoryValue &right);

// All equality/shape/union decisions live here; encoders and typed input share these rules.
// Same source revision, actor, purpose and read Grant; excludes the separately acquired export.
bool same_memory_read(const MemoryDependency &a, const MemoryDependency &b);
bool same_dependency(const MemoryDependency &left, const MemoryDependency &right);
// Checks scalar identities, optional Grant pairs and known purpose/kind.
bool valid_dependency(const MemoryDependency &dependency);
// Adds an exact duplicate once; a different revision of the same source is a conflict.
// Failure preserves the entire set. Grant changes never replace a captured permission.
ayni_status_t add_dependency(MemoryDependencies &set, const MemoryDependency &dependency,
                             uint32_t limit);
// Monotone subset: existing export Grants stay exact; a read without export may gain its first.
bool contains_dependencies(const MemoryDependencies &set, const MemoryDependencies &subset);
// Merge atomically, applying the same source/authorization union rule.
ayni_status_t merge_dependencies(MemoryDependencies &set, const MemoryDependencies &input,
                                 uint32_t limit);
// Validate full sets and exact unique labels without mutating caller input.
bool valid_dependencies(const MemoryDependencies &set, uint32_t limit);
// Decode the two ABI label arrays once; no caller body or pointer is retained.
bool read_memory_labels(const ayni_string_v1 *tags, uint32_t tag_count,
                        const ayni_string_v1 *keywords, uint32_t keyword_count, MemoryLabels &out);
// Reject duplicates or out-of-range tag/keyword counts; entries after counts are ignored.
bool valid_memory_labels(const MemoryLabels &labels);
} // namespace ayni::knowledge
#endif
