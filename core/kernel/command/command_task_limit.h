// Exact owner command identity for raising a Task's model ceiling (C-RUN-003).
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_TASK_LIMIT_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_TASK_LIMIT_H

#include "kernel/command/exact_command.h"

#include <ayni/ayni.h>
#include <cstdint>

namespace ayni::kernel {
// Pointer-free canonical request; session is deliberately outside retry identity.
struct TaskLimitCommand {
    uint32_t expected_revision; // Positive Task CAS before the raise.
    uint64_t task_id;           // Positive retained Task ID; never reused.
    uint32_t limit;             // Positive new total call ceiling, not additional calls.
    // Exact semantic identity before byte encoding.
    bool operator==(const TaskLimitCommand &other) const {
        return expected_revision == other.expected_revision && task_id == other.task_id &&
               limit == other.limit;
    }
};
// records.md: kind/u32, revision/u32, Task/u64, limit/u32, little endian.
constexpr uint32_t task_limit_command_size = 20;
// One shape validator for typed and byte inputs; current Task checks belong to work.
ayni_status_t measure_task_limit_command(const TaskLimitCommand &input, uint32_t &size);
// Writes only after shape/capacity checks; written changes only on success.
ayni_status_t encode_task_limit_command(const TaskLimitCommand &input, uint8_t *out,
                                        uint32_t capacity, uint32_t &written);
// Exact size/kind and shared shape checks; failure preserves out.
bool read_task_limit_command(ayni_bytes_v1 bytes, TaskLimitCommand &out);
template <> struct ExactCommand<TaskLimitCommand> {
    static constexpr ayni_intent_kind_t kind(const TaskLimitCommand &) {
        return AYNI_INTENT_TASK_RAISE_MODEL_LIMIT;
    }
    static constexpr auto measure = measure_task_limit_command;
    static constexpr auto encode = encode_task_limit_command;
};
} // namespace ayni::kernel
#endif
