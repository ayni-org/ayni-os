// Exact owner command identity for owner-managed target changes (ADR-0045).
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_TARGET_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_TARGET_H

#include "kernel/service/command/exact_command.h"

#include <ayni/ayni.h>
#include <cstdint>

namespace ayni::kernel {
// Pointer-free canonical request; session is deliberately outside retry identity.
struct TargetCommand {
    ayni_intent_kind_t kind;    // Query or resource-wait resume; determines the target owner.
    uint32_t expected_revision; // Positive target CAS before acceptance.
    uint64_t target_id;         // Positive retained target ID; never reused.
    // Exact semantic identity before byte encoding.
    bool operator==(const TargetCommand &other) const {
        return kind == other.kind && expected_revision == other.expected_revision &&
               target_id == other.target_id;
    }
};
// Recognized exact target commands share one encoding and typed validator.
inline bool is_target_command(ayni_intent_kind_t kind) {
    return kind == AYNI_INTENT_CALL_QUERY_RESULT || kind == AYNI_INTENT_TASK_RESUME_WAIT;
}
// records.md: kind/u32, revision/u32, target/u64, little endian.
constexpr uint32_t target_command_size = 16;
// One shape validator for typed and byte inputs; current target checks belong to work.
ayni_status_t measure_target_command(const TargetCommand &input, uint32_t &size);
// Writes only after shape/capacity checks; written changes only on success.
ayni_status_t encode_target_command(const TargetCommand &input, uint8_t *out, uint32_t capacity,
                                    uint32_t &written);
// Exact size/kind and shared shape checks; failure preserves out.
bool read_target_command(ayni_bytes_v1 bytes, TargetCommand &out);
// The request carries its own kind; measure refuses any other.
template <> struct ExactCommand<TargetCommand> {
    static constexpr ayni_intent_kind_t kind(const TargetCommand &request) { return request.kind; }
    static constexpr auto measure = measure_target_command;
    static constexpr auto encode = encode_target_command;
};
} // namespace ayni::kernel
#endif
