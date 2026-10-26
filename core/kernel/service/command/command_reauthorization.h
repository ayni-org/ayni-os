// Exact reauthorization request identity, independent of current work/catalog.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_REAUTHORIZATION_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_REAUTHORIZATION_H

#include "kernel/service/command/command_goal_identity.h"
#include "kernel/service/command/exact_command.h"

namespace ayni::kernel {

// Borrowed original request; authorization CAS is distinct from the user revision.
struct GoalReauthorization {
    uint64_t goal_id = 0; // Positive exact Goal ID; 0 is invalid.
    uint64_t expected_authority_commit_seq =
        0;                 // Positive last fact sequence, not binding generation.
    GoalCreate original{}; // Caller-owned immutable creation echo.
};
struct ReauthorizationCommand {
    uint32_t expected_revision = 0; // Positive Goal CAS; no live Goal lookup during exact retry.
    GoalReauthorization reauthorization; // Complete borrowed request identity.
};
// records.md: kind/revision (u32), Goal ID/authority CAS (u64), then original echo.
constexpr uint32_t reauthorization_command_prefix_size = 24;
constexpr uint32_t reauthorization_command_fixed_size =
    reauthorization_command_prefix_size + goal_identity_fixed_size;
constexpr uint32_t reauthorization_command_max_bytes(uint32_t parameters) {
    return reauthorization_command_prefix_size + goal_identity_max_bytes(parameters);
}
// Shared shape/measure for struct and byte input; failure preserves out_size.
ayni_status_t measure_reauthorization_command(const ReauthorizationCommand &input,
                                              uint32_t &out_size);
// Complete bounded write; BUFFER_TOO_SMALL changes only the required size.
ayni_status_t encode_reauthorization_command(const ReauthorizationCommand &input, uint8_t *body,
                                             uint32_t capacity, uint32_t &out_size);
// Strict canonical layout; publishes borrowed views only on complete success.
bool read_reauthorization_command(ayni_bytes_v1 bytes, ReauthorizationCommand &out);
template <> struct ExactCommand<ReauthorizationCommand> {
    static constexpr ayni_intent_kind_t kind(const ReauthorizationCommand &) {
        return AYNI_INTENT_GOAL_REAUTHORIZE;
    }
    static constexpr auto measure = measure_reauthorization_command;
    static constexpr auto encode = encode_reauthorization_command;
};
} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_REAUTHORIZATION_H
