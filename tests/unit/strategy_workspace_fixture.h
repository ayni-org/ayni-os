// Strategy test scratch is distinct from Core codec and result-rendering regions.
#ifndef AYNI_TEST_STRATEGY_WORKSPACE_FIXTURE_H
#define AYNI_TEST_STRATEGY_WORKSPACE_FIXTURE_H
#include "workspace_fixture.h"
#include <ayni/call_result_json.hpp>
#include <ayni/strategies/builtin.h>
namespace ayni::test {
inline const ayni_workspace_v1 *conversation_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (!memory.get() &&
        memory.create(::ayni::strategies::Conversation::workspace_requirements) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
inline const ayni_workspace_v1 *loop_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (!memory.get() &&
        memory.create(::ayni::strategies::ToolLoop::workspace_requirements) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
inline const ayni_workspace_v1 *result_workspace() {
    static thread_local ::ayni::Workspace memory;
    if (!memory.get() &&
        memory.create(::ayni::json::call_result_workspace_requirements) != AYNI_OK) {
        std::abort();
    }
    return memory.get();
}
} // namespace ayni::test
#endif
