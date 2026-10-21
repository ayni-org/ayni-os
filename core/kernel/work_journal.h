// What each transition writes to the log (contracts/storage/records.md,
// ADR-0026): the records of every object it created or changed, found by
// the stamp it gave them, so the rules that change the work model need not
// know about the log. A transition creates at most one goal and one task,
// and decides at most one task, which bounds its batch by the capacities
// alone.

#ifndef AYNI_CORE_KERNEL_WORK_JOURNAL_H
#define AYNI_CORE_KERNEL_WORK_JOURNAL_H

#include "durability/batch/batch_codec.h"
#include "foundation/transition_stamp.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_config.h"

#include <cstdint>

namespace ayni::kernel {

// The largest batch a transition writes under these capacities: every
// instance, goal and task changed, one goal and one task created with the
// largest parameters and ids, the most steps one decision adds, the largest
// checkpoint and one command. The capacities are within the ayni.h maxima,
// so it stays below AYNI_STORAGE_BUFFER_BYTES_MAX.
uint32_t transition_batch_max(const KernelCapacities &capacities);
// The smallest storage buffer that holds such a batch in the journal ring.
uint32_t storage_buffer_min(const KernelCapacities &capacities);

// Encodes the batch of the transition stamp, the latest the model applied,
// into writer and returns its encoded size; 0 when it did not fit, a defect
// (AYNI_RESTRICTION_INTERNAL).
std::size_t encode_transition(const WorkModel &model, foundation::TransitionStamp stamp,
                              durability::BatchWriter &writer);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_WORK_JOURNAL_H
