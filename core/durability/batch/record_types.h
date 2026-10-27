// Every record type of the log and the snapshot payload (contracts/storage/
// records.md, ADR-0051). The module that owns a record names its type from
// here and keeps the body's meaning, so two owners cannot take the same
// number: a new record type takes the next free number in this list first.

#ifndef AYNI_CORE_DURABILITY_BATCH_RECORD_TYPES_H
#define AYNI_CORE_DURABILITY_BATCH_RECORD_TYPES_H

#include <cstddef>
#include <cstdint>

namespace ayni::durability::record_type {

constexpr uint16_t boot_epoch = 1;                 // kernel/boot
constexpr uint16_t instance = 2;                   // work/agent
constexpr uint16_t goal = 3;                       // work/goal
constexpr uint16_t goal_state = 4;                 // work/goal
constexpr uint16_t task = 5;                       // work/task
constexpr uint16_t task_state = 6;                 // work/task
constexpr uint16_t task_steps = 7;                 // work/task
constexpr uint16_t task_checkpoint = 8;            // work/task
constexpr uint16_t command_mapping = 9;            // kernel/command
constexpr uint16_t writer_fence = 10;              // durability/batch
constexpr uint16_t snapshot_commit = 11;           // durability/snapshot
constexpr uint16_t owner_binding = 12;             // authority/principal
constexpr uint16_t grant = 13;                     // authority/grant
constexpr uint16_t grant_state = 14;               // authority/grant
constexpr uint16_t grant_revocation = 15;          // authority/grant
constexpr uint16_t grant_revocation_progress = 16; // authority/grant
constexpr uint16_t goal_authority = 17;            // work/goal
constexpr uint16_t task_authority = 18;            // work/task
constexpr uint16_t authority_cleanup = 19;         // work/authority
constexpr uint16_t action_state = 20;              // work/action
constexpr uint16_t call = 21;                      // work/call
constexpr uint16_t provenance = 24;                // work/provenance
constexpr uint16_t privacy_cleanup = 25;           // work/privacy
constexpr uint16_t calendar = 27;                  // work/goal
constexpr uint16_t context = 26;                   // knowledge/context
constexpr uint16_t memory = 23;                    // knowledge/memory
constexpr uint16_t scene = 22;                     // authority/scene

constexpr uint16_t all[] = {
    boot_epoch,
    instance,
    goal,
    goal_state,
    task,
    task_state,
    task_steps,
    task_checkpoint,
    command_mapping,
    writer_fence,
    snapshot_commit,
    owner_binding,
    grant,
    grant_state,
    grant_revocation,
    grant_revocation_progress,
    goal_authority,
    task_authority,
    authority_cleanup,
    action_state,
    call,
    scene,
    memory,
    provenance,
    privacy_cleanup,
    context,
    calendar,
};

constexpr bool all_distinct() {
    for (std::size_t first = 0; first < sizeof(all) / sizeof(all[0]); ++first) {
        for (std::size_t second = first + 1U; second < sizeof(all) / sizeof(all[0]); ++second) {
            if (all[first] == all[second]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(all_distinct(), "each record type has one owner");

} // namespace ayni::durability::record_type

#endif // AYNI_CORE_DURABILITY_BATCH_RECORD_TYPES_H
