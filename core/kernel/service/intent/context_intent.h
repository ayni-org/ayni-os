// Owner context management after shared Session/exact-command/storage admission.
#ifndef AYNI_CORE_KERNEL_CONTEXT_INTENT_H
#define AYNI_CORE_KERNEL_CONTEXT_INTENT_H
#include "kernel/service/intent/intent.h"
namespace ayni::kernel {
ayni_status_t apply_context_intent(WorkModel &model, const ContextCommand &command, uint64_t now_us,
                                   uint64_t durable_seq, IntentOutcome &outcome);
} // namespace ayni::kernel
#endif
