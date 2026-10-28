// White-box E3/E4 evidence only: queries missing from the public ABI. Every
// mutation/restart still goes through public interfaces and the Sim Host.
#include "../unit/kernel_test_probe.h"
#include "experiment_composite.h"
#include "experiment_support.h"
#include "work/task/task_records.h"
namespace ayni::runtime::experiments {
ayni_status_t inspect_call_acceptance(const ayni_kernel_t *kernel, uint64_t id, uint64_t &out) {
    const auto &calls = kernel::KernelTestProbe::work(kernel).calls;
    uint32_t index = 0;
    if (!calls.find(id, index)) {
        return AYNI_NOT_FOUND;
    }
    out = calls.state(index).created.transition.commit_seq;
    return AYNI_OK;
}
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
bool record_reader_versions(World &world) {
    auto &e = world.evidence;
    const auto &catalogs = kernel::KernelTestProbe::work(world.node.kernel()).catalogs;
    catalog::CatalogId strategy_id;
    uint32_t index = 0;
    if (!e.equal("probe strategy ID valid", strategy_id.assign(text("composite.policy")), 1) ||
        !e.equal("probe reader strategy registered", catalogs.strategies.find(strategy_id, index),
                 1)) {
        return false;
    }
    e.fact("reader", "strategy", catalogs.strategies.at(index).version);
    constexpr uint32_t future_version = 2;
    auto type = catalogs.goal_types.find(text("composite_workflow"), 1);
    if (type.kind != catalog::GoalTypeRegistry::Lookup::Kind::found) {
        type = catalogs.goal_types.find(text("composite_workflow"), future_version);
    }
    if (!e.equal("probe exact reader GoalType registered",
                 type.kind == catalog::GoalTypeRegistry::Lookup::Kind::found, 1)) {
        return false;
    }
    e.fact("reader", "goal_type", catalogs.goal_types.at(type.index).version);
    for (uint32_t i = 0; i < catalogs.bindings.size(); ++i) {
        const auto &binding = catalogs.bindings.at(i);
        if (binding.id.equals(text("increment"))) {
            e.fact("reader", "binding", binding.version);
            e.fact("reader", "capability", binding.capability_version);
        }
    }
    const bool resolver = catalogs.providers.find(text("experiment.counter"), 1, index);
    e.fact("reader", "query_resolver",
           resolver && catalogs.providers.at(index).query ? catalogs.providers.at(index).version
                                                          : 0);
    e.fact("reader", "storage", durability::storage_schema);
    e.fact("reader", "required_task_state_max", work::task_state_version_max);
    // A supported current image can be read with the owner codec. Unsupported
    // schema bytes are instead recorded by the version-injection owner.
    uint16_t record_version = 0;
    for (const auto &stored : world.node.simulation()->durable_storage().log) {
        durability::BatchView batch{};
        const auto checked =
            durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch);
        if (checked == durability::BatchCheck::unsupported) {
            continue;
        }
        if (!e.equal("matrix media framing valid", checked == durability::BatchCheck::ok, 1)) {
            return false;
        }
        e.fact("writer", "storage", batch.schema);
        durability::RecordReader reader(batch);
        durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        auto next = reader.next(header, body);
        while (next == durability::RecordReader::Next::record) {
            if (header.type == durability::record_type::task_state &&
                header.version > record_version) {
                record_version = header.version;
            }
            next = reader.next(header, body);
        }
        if (!e.equal("matrix record framing complete", next == durability::RecordReader::Next::end,
                     1)) {
            return false;
        }
    }
    if (record_version) {
        e.fact("writer", "required_task_state", record_version);
    }
    return true;
}
} // namespace ayni::runtime::experiments
