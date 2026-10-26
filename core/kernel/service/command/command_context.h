// Exact owner context operations have bounded scalar identity and retain no plaintext.
#ifndef AYNI_CORE_KERNEL_COMMAND_CONTEXT_H
#define AYNI_CORE_KERNEL_COMMAND_CONTEXT_H
#include "kernel/service/command/exact_command.h"
#include "knowledge/context/context_table.h"
#include <ayni/context.h>
namespace ayni::kernel {
struct ContextCommand {
    ayni_intent_kind_t kind = 0;    // OPEN/PUBLISH/FORGET/ACK, each with one exact shape.
    uint32_t expected_revision = 0; // Zero only for OPEN; otherwise current privacy revision.
    uint64_t owner_instance_id = 0; // OPEN only, resolved Agent identity.
    uint64_t context_id = 0;        // Existing anchor for non-OPEN operations.
    uint64_t generation = 0;        // Exact current file generation; zero is valid initially.
    uint64_t task_id = 0;           // PUBLISH only, completed durable Task of the anchor owner.
    uint64_t turn_id = 0; // Next published turn or exact deleted turn; zero for all/ACK/OPEN.
    knowledge::ContextAction action = knowledge::ContextAction::none; // FORGET only, all or round.
};
constexpr uint32_t context_command_size =
    56; // records.md: kind/revision, five u64, action/reserved.
constexpr bool is_context_command(ayni_intent_kind_t kind) {
    return kind >= AYNI_INTENT_CONTEXT_OPEN && kind <= AYNI_INTENT_CONTEXT_ACK;
}
ayni_status_t decode_context_command(const ayni_intent_v1 &raw, ContextCommand &out);
ayni_status_t measure_context_command(const ContextCommand &command, uint32_t &size);
ayni_status_t encode_context_command(const ContextCommand &command, uint8_t *out, uint32_t capacity,
                                     uint32_t &written);
bool read_context_command(ayni_bytes_v1 bytes, ContextCommand &out);
template <> struct ExactCommand<ContextCommand> {
    static constexpr ayni_intent_kind_t kind(const ContextCommand &command) { return command.kind; }
    static constexpr auto measure = measure_context_command;
    static constexpr auto encode = encode_context_command;
};
} // namespace ayni::kernel
#endif
