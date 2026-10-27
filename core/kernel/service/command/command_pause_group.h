// Exact bounded pause-group controls, independent of Session and live catalogs.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_PAUSE_GROUP_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_PAUSE_GROUP_H
#include "kernel/service/command/exact_command.h"
#include "work/goal/pause_groups.h"
namespace ayni::kernel {
// records.md type9/v14 canonical: kind, expected revision, group, desired pause (u32).
constexpr uint32_t pause_group_command_size = 16;
ayni_status_t measure_pause_group_command(const work::PauseGroupChange &, uint32_t &size);
ayni_status_t encode_pause_group_command(const work::PauseGroupChange &, uint8_t *body,
                                         uint32_t capacity, uint32_t &written);
bool read_pause_group_command(ayni_bytes_v1 bytes, work::PauseGroupChange &out);
// Group commands use the existing exact command window and complete-byte comparison.
template <> struct ExactCommand<work::PauseGroupChange> {
    static constexpr ayni_intent_kind_t kind(const work::PauseGroupChange &) {
        return AYNI_INTENT_PAUSE_GROUP_SET;
    }
    static constexpr auto measure = measure_pause_group_command;
    static constexpr auto encode = encode_pause_group_command;
};
} // namespace ayni::kernel
#endif
