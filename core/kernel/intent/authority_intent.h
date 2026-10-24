#ifndef AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H

#include "kernel/intent/work_model.h"

namespace ayni::kernel {

// Current durable owner binding and authenticated management channel.
bool bound_management_session(const WorkModel &model, const authority::Session &session,
                              uint64_t durable_seq);
// All missing Grants, retained dependencies and activation in one transition.
ayni_status_t approve_goal(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure);

// Session/catalog/subject resolution and one atomic Grant transition. The
// caller has reserved command space and staged the exact command identity.
ayni_status_t create_grant(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure);

// Acceptance reserves a volatile control and its deny gate before storage
// admission. Applying it later uses the accepted identity, never a Session
// or a second revision check.
ayni_status_t accept_revoke(WorkModel &model, const Intent &intent,
                            const authority::Session &session, uint64_t durable_seq,
                            IntentOutcome &outcome, IntentFailure &failure);
bool apply_pending_revoke(WorkModel &model);
bool apply_revocation_cleanup(WorkModel &model);
// Copy PENDING/APPLIED from accepted control, leaving orchestration flags
// untouched. An APPLIED control requires its pinned mapping; receipt adds DURABLE.
void pending_revoke_outcome(const PendingRevoke &pending, const CommandMapping *mapping,
                            IntentOutcome &outcome);

} // namespace ayni::kernel
#endif
