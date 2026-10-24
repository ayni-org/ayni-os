// Work-owned admission evidence (ADR-0038). Values name past decisions;
// they never cache a Grant's current state or consult a Host clock.
#ifndef AYNI_CORE_WORK_AUTHORITY_AUTHORITY_FACTS_H
#define AYNI_CORE_WORK_AUTHORITY_AUTHORITY_FACTS_H
#include "foundation/transition_stamp.h"

#include <cstdint>

namespace ayni::work {

// Immutable selected identity, not the current revision of a mutable Grant.
struct AuthorityReference {
    uint64_t grant_id = 0; // Kernel ID; 0 iff revision is 0, meaning not selected.
    uint32_t revision = 0; // Nonzero selected revision iff grant_id is present.
};

// A Goal's admitted binding, distinct from subsequent pause/change stamps.
struct BindingReference {
    uint64_t goal_id = 0;    // Exact Goal ID, or 0 iff commit_seq is 0.
    uint64_t commit_seq = 0; // Binding log sequence; the zero pair means none.
};

// A resume's logical input frontier, never a timestamp or queued input copy.
struct InputResumeBoundary {
    uint64_t incarnation = 0; // Ingress's first durable boot epoch; 0 means no boundary.
    uint64_t through = 0;     // Inclusive accepted-input sequence; 0 is an empty frontier.
};

// Goal-owned evidence; ordinary user pause does not change established_seq.
struct GoalBinding {
    uint64_t established_seq = 0; // Positive admission sequence, no later than its update stamp.
    BindingReference parent;      // None for roots; otherwise the admitted parent binding.
    InputResumeBoundary input;    // Last explicit resume frontier; none before such a resume.
};

// Shared reference shape, including the explicit missing pair used by proposals.
bool valid_authority_reference(const AuthorityReference &reference);
// A Task's complete admission cannot contain a missing reference.
bool valid_selected_authority_reference(const AuthorityReference &reference);
// None is the zero pair; present references require a valid Goal identity.
bool valid_binding_reference(const BindingReference &reference);
// Incarnation is an epoch, not a Kernel ID; through may be zero when present.
bool valid_input_resume_boundary(const InputResumeBoundary &boundary);
// Bound Goals name their actual parent; parent admission precedes child admission.
bool valid_goal_binding(const GoalBinding &binding, uint64_t goal_id, uint64_t parent_goal_id,
                        foundation::TransitionStamp stamp);
} // namespace ayni::work

#endif // AYNI_CORE_WORK_AUTHORITY_AUTHORITY_FACTS_H
