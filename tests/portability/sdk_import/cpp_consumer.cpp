// ADR-0089: public strategies and optional components link outside the root project.
#include <ayni/ayni.hpp>
#include <ayni/call_result_json.hpp>
#include <ayni/strategies/builtin.h>
#include <ayni/workspace.hpp>
#include <cstdio>
#ifdef PROBE_STORAGE
#include <ayni/hosts/storage/file_log.h>
#include <ayni/hosts/storage/file_value.h>
#endif
#ifdef PROBE_NATIVE_HOST
#include <ayni/hosts/native.h>
#include <cstdlib>
#include <filesystem>
#endif
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
    ayni::Workspace workspace;
    if (workspace.create(ayni::strategies::ToolLoop::workspace_requirements) != AYNI_OK) {
        return 1;
    }
    ayni::strategies::ToolLoop loop;
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = {"consumer.loop", sizeof("consumer.loop") - 1};
    config.model.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = {"consumer.model", sizeof("consumer.model") - 1};
    config.summary = config.model;
    config.limits = {32768, 1024, 8192, 4096};
    // Only configuration is probed here, so this body store never holds a body.
    static const ayni_body_store_v1 bodies{
        sizeof(ayni_body_store_v1),
        0,
        nullptr,
        [](void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *) { return AYNI_UNSUPPORTED; },
        [](void *, const ayni_body_ref_v1 *, uint8_t *, uint32_t) { return AYNI_NOT_FOUND; },
        [](void *, const ayni_body_ref_v1 *) {}};
    config.bodies = &bodies;
    if (loop.configure(config, workspace.get()) != AYNI_OK || loop.describe().ctx == nullptr) {
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
#ifdef PROBE_STORAGE
    if (ayni::hosts::storage::valid_file_value_request({}) ||
        ayni::hosts::storage::FileLog::append_bytes(1) <= 1) {
        return 1;
    }
#endif
#ifdef PROBE_NATIVE_HOST
    char directory[] = "/tmp/ayni-native-import-XXXXXX";
    if (!mkdtemp(directory)) {
        return 1;
    }
    bool storage_ok = false;
    {
        ayni::hosts::native::Host host;
        if (host.open_storage(directory, 2).status == ayni::hosts::native::StorageStatus::ok) {
            auto ports = host.table();
            uint64_t available = 0;
            storage_ok = host.value_store().submit && ports.storage_append &&
                         ports.storage_read_prefix && ports.storage_read_log &&
                         ports.storage_fence_writers && ports.storage_write_snapshot &&
                         ports.storage_read_snapshot && ports.storage_trim_prefix &&
                         ports.storage_free_bytes &&
                         ports.storage_free_bytes(ports.ctx, &available) == AYNI_OK && available;
        }
        host.close_storage();
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (!storage_ok || error) {
        return 1;
    }
#endif
    return 0;
}
