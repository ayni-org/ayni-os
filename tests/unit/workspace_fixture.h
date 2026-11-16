// ABI 0.53 test-owned memory, separate for public values, proof callbacks and
// internal codec calls. Production callers provide their own explicit owners.
#ifndef AYNI_TEST_WORKSPACE_FIXTURE_H
#define AYNI_TEST_WORKSPACE_FIXTURE_H
#include "foundation/workspace.h"
#include <array>
#include <ayni/calls.h>
#include <ayni/workspace.hpp>
#include <cstdlib>
#include <optional>
#include <vector>

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
// Test-owned decoded proposals. Strings borrow the caller's immutable list body.
struct ToolCalls {
    struct Item {
        ayni_tool_call_v1 call{};
        std::vector<ayni_argument_node_v1> nodes;
    };
    std::array<Item, AYNI_CALL_TOOL_COUNT_MAX> calls;
    uint32_t count = 0;
    ayni_string_v1 assistant{};
    ayni_status_t read(ayni_bytes_v1 bytes) {
        return ayni_tool_calls_read(
            tool_calls_workspace(), bytes, this,
            [](void *context, uint32_t index, const ayni_tool_call_v1 *call) {
                auto &item = static_cast<ToolCalls *>(context)->calls[index];
                item.call = *call;
                if (call->input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
                    const auto &args = call->input.arguments;
                    item.nodes.assign(args.nodes, args.nodes + args.node_count);
                    item.call.input.arguments = {};
                }
                return AYNI_OK;
            },
            &count, &assistant);
    }
    ayni_tool_call_v1 at(uint32_t index) const {
        auto call = calls[index].call;
        if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
            const auto &nodes = calls[index].nodes;
            call.input.arguments = {nodes.data(), static_cast<uint32_t>(nodes.size())};
        }
        return call;
    }
};
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
