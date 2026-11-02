// ABI 0.53 test-owned memory, separate for public values, proof callbacks and
// internal codec calls. Production callers provide their own explicit owners.
#ifndef AYNI_TEST_WORKSPACE_FIXTURE_H
#define AYNI_TEST_WORKSPACE_FIXTURE_H
#include "foundation/workspace.h"
#include <ayni/calls.h>
#include <ayni/workspace.hpp>
#include <cstdlib>
#include <optional>

namespace ayni::test {
inline const ayni_workspace_v1 *value_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (memory.get() == nullptr && memory.create(::ayni::Workspace::Purpose::value) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
inline const ayni_workspace_v1 *tool_calls_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (memory.get() == nullptr &&
        memory.create(ayni_tool_calls_workspace_requirements) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
// One proposal of a completed inline TOOL_CALLS result, whose text holds the list.
inline ayni_status_t read_tool_call(const ayni_task_call_result_v1 &result, uint32_t index,
                                    ayni_task_tool_call_v1 *out) {
    return ayni_tool_call_read(
        tool_calls_workspace(),
        {reinterpret_cast<const uint8_t *>(result.text.data), result.text.size}, index, out);
}
inline const ayni_workspace_v1 *business_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (memory.get() == nullptr && memory.create(::ayni::Workspace::Purpose::business) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
inline foundation::Workspace &codec_workspace() {
    struct Owned {
        ::ayni::Workspace memory;
        std::optional<foundation::Workspace> arena;
        Owned() {
            if (memory.create(::ayni::Workspace::Purpose::business) != AYNI_OK) {
                std::abort();
            }
            arena.emplace(memory.get()->data, memory.get()->byte_capacity);
        }
    };
    static thread_local Owned owner;
    return *owner.arena;
}
} // namespace ayni::test
#endif
