// Immutable result evidence for approval/reauthorization commands (ADR-0038).
// Command owns the historical copy; Goal retains the immutable Scope ceiling.
#ifndef AYNI_CORE_KERNEL_COMMAND_WORK_AUTHORITY_RESULT_H
#define AYNI_CORE_KERNEL_COMMAND_WORK_AUTHORITY_RESULT_H

#include "work/authority/authority_facts.h"
#include "work/goal/goal_authority.h"

#include <array>
#include <ayni/ayni.h>

namespace ayni::kernel {

// These exact command kinds create a new binding and must retain its result.
constexpr bool is_work_authority_command(ayni_intent_kind_t kind) {
    return kind == AYNI_INTENT_GOAL_APPROVE || kind == AYNI_INTENT_GOAL_REAUTHORIZE;
}

// One binding decision's result, independent of the Goal's later selections.
struct WorkAuthorityResult {
    uint64_t transition_id = 0; // Positive transition establishing binding; 0 only before use.
    work::GoalBinding binding;  // Copied result; established_seq is that transition's log sequence.
    uint32_t count = 0;         // Ordered selected prefix; explicit zero requirements is valid.
    // Copied Grant identities in original requirement order; unused tail is outside the evidence.
    std::array<work::AuthorityReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> references{};
};

// records.md type9/v5 result: transition ID, five binding u64s, count u32,
// then count selected Grant ID/revision pairs. No Scope or monotonic time.
constexpr uint32_t work_authority_result_fixed_size = 52;
constexpr uint32_t work_authority_result_reference_size = 12;
constexpr uint32_t work_authority_result_size(uint32_t count) {
    return work_authority_result_fixed_size + count * work_authority_result_reference_size;
}

// Snapshot coverage bounds every saved historical result, including superseded
// generations and commands whose target has been reclaimed.
bool work_authority_result_within(const WorkAuthorityResult &result, uint64_t transition_limit,
                                  uint64_t boot_epoch_limit);
// Shared struct/byte shape rule; mapping supplies its exact result Goal ID.
bool valid_work_authority_result(const WorkAuthorityResult &result, uint64_t goal_id);
// Copy a just-established authorized binding, whose stamp is its admission
// stamp. Paused/later current facts cannot reconstruct a historical result.
WorkAuthorityResult copy_work_authority_result(const work::GoalAuthorityRecord &record);
// Exact byte equality with a just-published Goal binding, using the same
// canonical writer as persistence; caller retains unchanged record views.
bool work_authority_result_matches(const work::GoalAuthorityRecord &record, ayni_bytes_v1 bytes);
// A retained current generation must agree with its saved result even after
// a later pause stamp; a strictly newer binding has independent result evidence.
bool retains_work_authority_result(const work::GoalAuthorityRecord &record,
                                   const WorkAuthorityResult &result);
// Caller proved shape and owns exactly result_size(count) writable bytes.
void write_work_authority_result(const WorkAuthorityResult &result, uint8_t *body);
// Exact bounded reader, no live Goal/Grant lookup; failure preserves out.
bool read_work_authority_result(ayni_bytes_v1 bytes, uint64_t goal_id, WorkAuthorityResult &out);

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_WORK_AUTHORITY_RESULT_H
