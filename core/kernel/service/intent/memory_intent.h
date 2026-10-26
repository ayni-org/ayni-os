// Owner memory transitions after shared Session/command/storage admission.
#ifndef AYNI_CORE_KERNEL_MEMORY_INTENT_H
#define AYNI_CORE_KERNEL_MEMORY_INTENT_H
#include "kernel/service/intent/intent.h"
namespace ayni::kernel {
// Exact user save/confirm/delete; every fallible check precedes IDs and mutation.
ayni_status_t apply_memory_intent(WorkModel &model, const MemoryCommand &command, uint64_t now_us,
                                  uint64_t durable_seq, IntentOutcome &outcome);
} // namespace ayni::kernel
#endif
