#ifndef AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_AUTHORITY_INTENT_H

#include "kernel/intent/work_model.h"

namespace ayni::kernel {

// Session/catalog/subject resolution and one atomic Grant transition. The
// caller has reserved command space and staged the exact command identity.
ayni_status_t create_grant(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us,
                           uint64_t durable_seq, IntentOutcome &outcome,
                           IntentFailure &failure);

} // namespace ayni::kernel
#endif
