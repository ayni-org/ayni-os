// Atomic independent gate controls; no application policy or physical-stop claims.
#ifndef AYNI_CORE_KERNEL_INTENT_PAUSE_GROUP_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_PAUSE_GROUP_INTENT_H
#include "kernel/service/intent/intent.h"
#include "kernel/service/model/work_model.h"
namespace ayni::kernel {
// Caller authenticated management and reserved exact command/journal space.
ayni_status_t set_pause_group(WorkModel &, const work::PauseGroupChange &, uint64_t now_us,
                              InputResumeSampler input, IntentOutcome &, IntentFailure &);
} // namespace ayni::kernel
#endif
