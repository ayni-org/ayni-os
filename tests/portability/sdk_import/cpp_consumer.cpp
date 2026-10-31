// ADR-0089: public strategies and optional components link outside the root project.
#include <ayni/ayni.hpp>
#include <ayni/strategies/builtin.h>
#include <cstdio>
#ifdef PROBE_SOFTWARE
#include <ayni/providers/software/work.h>
#endif
#ifdef PROBE_GATE
#include <ayni/control.h>
#endif
#ifdef PROBE_ASSISTANT
#include <ayni/applications/assistant.h>
#endif
#ifdef PROBE_WORKFLOW
#include <ayni/applications/software_workflow.h>
#endif
#ifdef PROBE_MOCK
#include <ayni/providers/mock.h>
#endif
#ifdef PROBE_EXECUTION
#include <ayni/providers/mock_execution.h>
#endif

int main() {
    ayni::Kernel kernel;
    if (kernel.close() != AYNI_OK) {
        std::puts("consumer: kernel close");
        return 1;
    }
    ayni::strategies::ToolLoop loop;
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = {"consumer.loop", sizeof("consumer.loop") - 1};
    config.model.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = {"consumer.model", sizeof("consumer.model") - 1};
    config.summary = config.model;
    if (loop.configure(config) != AYNI_OK || loop.describe().ctx == nullptr) {
        std::puts("consumer: ToolLoop configure");
        return 1;
    }
#ifdef PROBE_SOFTWARE
    ayni::providers::software::WorkProvider software;
    if (software.close() != AYNI_OK) {
        std::puts("consumer: software close");
        return 1;
    }
#endif
#ifdef PROBE_GATE
    size_t gate_bytes = 0, gate_alignment = 0;
    if (ayni_control_memory_requirements(&gate_bytes, &gate_alignment) != AYNI_OK ||
        gate_bytes == 0 || gate_alignment == 0) {
        std::puts("consumer: gate requirements");
        return 1;
    }
#endif
#ifdef PROBE_ASSISTANT
    const ayni::applications::assistant::QuietHours quiet;
    if (quiet.validate() != AYNI_OK) {
        std::puts("consumer: quiet validation");
        return 1;
    }
#endif
#ifdef PROBE_WORKFLOW
    if (ayni::applications::software_workflow::configure(nullptr, nullptr) !=
        AYNI_INVALID_ARGUMENT) {
        std::puts("consumer: workflow validation");
        return 1;
    }
#endif
#ifdef PROBE_MOCK
    ayni::providers::mock::Provider mock;
    if (mock.configure({}) != AYNI_OK || mock.close() != AYNI_OK) {
        std::puts("consumer: mock close");
        return 1;
    }
#endif
#ifdef PROBE_EXECUTION
    ayni::providers::mock_execution::Executor executor;
    if (executor.close() != AYNI_OK) {
        std::puts("consumer: executor close");
        return 1;
    }
#endif
    return 0;
}
