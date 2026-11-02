// Composition of bounded privacy effects; each body remains owned by its original table.
#ifndef AYNI_CORE_KERNEL_MEMORY_PRIVACY_SERVICE_H
#define AYNI_CORE_KERNEL_MEMORY_PRIVACY_SERVICE_H
#include "kernel/service/model/work_model.h"
namespace ayni::kernel {
// Source revisions/tombstones only: temporary Grant/catalog unavailability never erases content.
bool content_sources_retained(const WorkModel &model,
                              const knowledge::MemoryDependencies &dependencies);
// Structural anchor check over its complete closure; no Grant expiry or wall clock participates.
bool context_sources_retained(const WorkModel &model, uint32_t index);
// Report exhausted privacy/storage generations explicitly; zero work remains falsely completed.
ayni_status_t privacy_progress_status(const WorkModel &model, uint64_t generation);
// Select one stable-identity scan step. Storage/file/transport readiness is supplied by runtime.
// The result has no stamp until the caller admits and publishes its transition.
bool next_privacy_step(const WorkModel &model, uint64_t generation, work::PrivacyRecord &out);
// Apply a previously selected/proved effect and its cursor atomically; no I/O or callbacks.
// Replay applies a persisted step without queueing body releases the original run queued.
bool apply_privacy_step(WorkModel &model, const work::PrivacyRecord &record, uint64_t boot_epoch,
                        bool replay = false);
// Advance exactly one selected item after runtime's external completion gates hold.
bool advance_privacy(WorkModel &model, uint64_t generation);
} // namespace ayni::kernel
#endif
