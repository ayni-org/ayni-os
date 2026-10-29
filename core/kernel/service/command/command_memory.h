// Exact owner memory requests: preserve bytes until privacy cleanup explicitly expires the ID.
#ifndef AYNI_CORE_KERNEL_COMMAND_MEMORY_H
#define AYNI_CORE_KERNEL_COMMAND_MEMORY_H
#include "kernel/service/command/exact_command.h"
#include "knowledge/memory/memory_records.h"
#include <ayni/memory.h>
namespace ayni::kernel {
// Kind/revision/target followed by the shared value for SAVE; records.md type9/v10.
constexpr uint32_t memory_command_fixed_bytes = 16;
// Borrowed explicit management request. CONFIRM/DELETE carry no replacement value.
struct MemoryCommand {
    ayni_intent_kind_t kind = 0;    // SAVE, CONFIRM or DELETE only.
    uint32_t expected_revision = 0; // Zero only for SAVE creating a new record.
    uint64_t target_id = 0;         // Zero only for a new SAVE.
    knowledge::MemoryValue value{}; // Complete explicit user value for SAVE; otherwise unused.
};
constexpr bool is_memory_command(ayni_intent_kind_t kind) {
    return kind >= AYNI_INTENT_MEMORY_SAVE && kind <= AYNI_INTENT_MEMORY_DELETE;
}
constexpr uint32_t memory_command_max(uint32_t body_bytes) {
    return body_bytes == 0
               ? 0
               : memory_command_fixed_bytes + knowledge::memory_value_fixed_bytes + body_bytes +
                     (knowledge::memory_tag_max + knowledge::memory_keyword_max) *
                         foundation::string_field_size(knowledge::memory_label_max_bytes);
}
// Decode ABI without retaining caller arrays; body stays borrowed until submission returns.
ayni_status_t decode_memory_command(const ayni_intent_v1 &raw, MemoryCommand &out);
// Intrinsic semantics shared with the canonical reader; no Kernel/Grant/catalog lookup.
ayni_status_t measure_memory_command(const MemoryCommand &input, uint32_t &size);
ayni_status_t encode_memory_command(const MemoryCommand &input, uint8_t *out, uint32_t capacity,
                                    uint32_t &written);
bool read_memory_command(ayni_bytes_v1 bytes, MemoryCommand &out);
template <> struct ExactCommand<MemoryCommand> {
    // The three owner operations share one canonical domain format.
    static constexpr ayni_intent_kind_t kind(const MemoryCommand &value) { return value.kind; }
    static constexpr auto measure = measure_memory_command; // Complete size/shape admission.
    static constexpr auto encode = encode_memory_command;   // Exact bytes, not a digest comparison.
};
} // namespace ayni::kernel
#endif
