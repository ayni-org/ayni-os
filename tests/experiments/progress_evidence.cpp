// White-box E3 evidence only: queries missing from the public ABI. Every
// mutation/restart still goes through public interfaces and the Sim Host.
#include "../unit/kernel_test_probe.h"
#include "experiment_support.h"
namespace ayni::runtime::experiments {
ayni_status_t inspect_task_progress(const ayni_kernel_t *kernel, uint64_t id,
                                    TaskProgressEvidence &out) {
    const auto &tasks = kernel::KernelTestProbe::work(kernel).tasks;
    uint32_t index = 0;
    if (!tasks.find(id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &task = tasks.at(index);
    TaskProgressEvidence value;
    value.writer.assign(task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size());
    value.writer_version = task.checkpoint_writer_version;
    const auto bytes = tasks.checkpoint(index);
    if (bytes.size) {
        value.state.assign(bytes.data, bytes.data + bytes.size);
    }
    for (uint32_t i = 0; i < task.step_count; ++i) {
        const auto &step = tasks.step(index, i);
        value.steps.emplace_back(step.data(), step.size());
    }
    out = std::move(value);
    return AYNI_OK;
}
ayni_status_t inspect_call_route(const ayni_kernel_t *kernel, uint64_t id, CallRouteEvidence &out) {
    const auto &calls = kernel::KernelTestProbe::work(kernel).calls;
    uint32_t index = 0;
    if (!calls.find(id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &call = calls.identity(index);
    const auto string = [](const catalog::CatalogId &v) { return std::string(v.data(), v.size()); };
    out = {string(call.binding_id), string(call.capability_id), string(call.provider_id),
           string(call.endpoint),   string(call.step_key),      call.binding_version,
           call.capability_version, call.provider_version};
    return AYNI_OK;
}
} // namespace ayni::runtime::experiments
