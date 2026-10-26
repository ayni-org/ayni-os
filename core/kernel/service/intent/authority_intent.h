#ifndef AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H

#include "kernel/service/intent/intent.h"
#include "kernel/service/model/work_model.h"

namespace ayni::kernel {

// Current durable owner binding and authenticated management channel.
bool bound_management_session(const WorkModel &model, const authority::Session &session,
                              uint64_t durable_seq);

// The operations below run after apply_intent has checked their session rule
// (intent_traits.h).

// All missing Grants, retained dependencies and activation in one transition.
ayni_status_t approve_goal(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure);
// Rebind existing durable authority for one Goal/direct live cohort; no implicit Grant creation.
ayni_status_t reauthorize_goal(WorkModel &model, const Intent &intent, uint64_t now_us,
                               uint64_t durable_seq, InputResumeSampler input,
                               IntentOutcome &outcome, IntentFailure &failure);

// Catalog/subject resolution and one atomic Grant transition. The caller has
// reserved command space and staged the exact command identity.
ayni_status_t create_grant(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure);

// Acceptance reserves a volatile control and its deny gate before storage
// admission. Applying it later uses the accepted identity, never a Session
// or a second revision check.
ayni_status_t accept_revoke(WorkModel &model, const Intent &intent,
                            const authority::Session &session, IntentOutcome &outcome,
                            IntentFailure &failure);
bool apply_pending_revoke(WorkModel &model);
bool apply_revocation_cleanup(WorkModel &model);
// Copy PENDING/APPLIED from accepted control, leaving orchestration flags
// untouched. An APPLIED control requires its pinned mapping; receipt adds DURABLE.
void pending_revoke_outcome(const PendingRevoke &pending, const CommandMapping *mapping,
                            IntentOutcome &outcome);

} // namespace ayni::kernel
#endif
