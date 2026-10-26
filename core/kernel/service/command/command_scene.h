// Exact Scene owner-command retry identity. Session authentication is not persisted.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_SCENE_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_SCENE_H
#include "authority/scene/scene_table.h"
#include "kernel/service/command/exact_command.h"
namespace ayni::kernel {
// records.md command v9 canonical body: kind/revision/mode/version and length-prefixed ID.
constexpr uint32_t scene_command_fixed_bytes = 20;
ayni_status_t measure_scene_command(const authority::SceneChange &input, uint32_t &size);
ayni_status_t encode_scene_command(const authority::SceneChange &input, uint8_t *out,
                                   uint32_t capacity, uint32_t &written);
bool read_scene_command(ayni_bytes_v1 bytes, authority::SceneChange &out);
template <> struct ExactCommand<authority::SceneChange> {
    static constexpr ayni_intent_kind_t kind(const authority::SceneChange &) {
        return AYNI_INTENT_SCENE_SET_ENABLED;
    }
    static constexpr auto measure = measure_scene_command;
    static constexpr auto encode = encode_scene_command;
};
} // namespace ayni::kernel
#endif
