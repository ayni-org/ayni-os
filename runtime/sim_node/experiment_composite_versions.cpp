#include "experiment_composite.h"
#include "experiment_execution.h"
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/observations.h>
namespace ayni::runtime::experiments {
// Every rejection goes through a fresh public registration arena. No callback
// can run before start validates the complete static selection (C-BIND-001).
bool Composite::registration_boundaries(CompositeScenario scenario) {
    auto &e = world_.evidence;
    if (scenario == CompositeScenario::unknown_goal_type_rejected) {
        auto body = initialized<ayni_goal_create_v1>();
        body.goal_type_id = text("unregistered.composite");
        body.goal_type_version = 1;
        body.owner_instance_id = world_.instance(1);
        body.trigger = AYNI_TRIGGER_ONCE;
        const auto before = world_.count(AYNI_OBJECT_GOAL);
        world_.submit(AYNI_INTENT_GOAL_CREATE, &body, 0, 0, AYNI_NOT_FOUND);
        return e.equal("unknown type creates no Goal", world_.count(AYNI_OBJECT_GOAL), before);
    }
    constexpr uint32_t invalid_cases = 5;
    for (uint32_t variant = 0; variant < invalid_cases; ++variant) {
        SimNode node;
        if (!e.status("assemble rejected registration", node.assemble(world_.config))) {
            return false;
        }
        auto capability = *model_offer_->capabilities;
        if (scenario == CompositeScenario::typed_schema_rejected) {
            ayni_status_t expected = AYNI_INVALID_ARGUMENT;
            switch (variant) {
            case 0:
                capability.contract.category = 0;
                break;
            case 1:
                capability.contract.timeout_ms = 0;
                break;
            case 2:
                capability.contract.input_text_limit = AYNI_CALL_TEXT_BYTES_MAX + 1;
                break;
            case 3:
                capability.contract.input_format = UINT32_MAX;
                expected = AYNI_UNSUPPORTED_VERSION;
                break;
            default:
                capability.contract.result_format = UINT32_MAX;
                expected = AYNI_UNSUPPORTED_VERSION;
                break;
            }
            if (!e.status("typed contract rejects invalid type/bound",
                          ayni_register_call_capability(node.kernel(), &capability), expected)) {
                return false;
            }
            capability = *model_offer_->capabilities;
            if (!e.status("rejected typed entry consumed no slot",
                          ayni_register_call_capability(node.kernel(), &capability))) {
                return false;
            }
        } else {
            const ayni_capability_desc_v1 authority{
                sizeof(authority), 1, capability.capability_id, 0, 0, nullptr};
            auto provider = *model_offer_;
            auto offered = capability;
            provider.capabilities = &offered;
            ayni_call_resource_v1 demand{text("model.slot"), 1, 0};
            capability.contract.resources = &demand;
            offered.contract.resources = &demand;
            const ayni_resource_desc_v1 resource{sizeof(resource),
                                                 AYNI_RESOURCE_SHARED,
                                                 demand.resource_id,
                                                 1,
                                                 0,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 {}};
            ayni_binding_v1 binding{sizeof(binding),
                                    1,
                                    text("bad.model"),
                                    capability.capability_id,
                                    1,
                                    1,
                                    provider.provider_id,
                                    provider.endpoint_id,
                                    {}};
            ayni_status_t expected = AYNI_INVALID_ARGUMENT;
            switch (variant) {
            case 0:
                offered.contract.result_text_limit -= 1;
                break; // Different result schema bound.
            case 1:
                binding.endpoint_id = text("missing.endpoint");
                break;
            case 2:
                binding.destination_id = text("ungranted.destination");
                break;
            case 3:
                demand.units = resource.capacity + 1;
                expected = AYNI_CAPACITY_EXCEEDED;
                break;
            default:
                offered.contract.category = AYNI_CALL_TOOL;
                break; // Modality/class mismatch.
            }
            if (!e.status("register rejected resource",
                          ayni_register_resource(node.kernel(), &resource)) ||
                !e.status("register rejected authority",
                          ayni_register_capability(node.kernel(), &authority)) ||
                !e.status("register declared contract",
                          ayni_register_call_capability(node.kernel(), &capability)) ||
                !e.status("register different offer",
                          ayni_register_provider(node.kernel(), &provider)) ||
                !e.status("register rejected Binding",
                          ayni_register_binding(node.kernel(), &binding)) ||
                !e.status("mismatched Binding fails before execution", ayni_start(node.kernel()),
                          expected)) {
                return false;
            }
        }
        if (!e.status("release rejected arena", node.release())) {
            return false;
        }
    }
    return e.equal("registration rejects cause no model submission", model_.pending(), 0) &&
           e.equal("registration rejects cause no physical sample",
                   world_.physical->executor.samples(), 0);
}
bool Composite::version_boundary(VersionCase kind) {
    auto &e = world_.evidence;
    const auto original = call("increment.v1");
    CallRouteEvidence original_route;
    if (!e.status(
            "inspect original writer semantics",
            inspect_call_route(world_.node.kernel(), original.logical_call_id, original_route))) {
        return false;
    }
    e.fact("writer", "capability", original_route.capability_version);
    e.fact("writer", "binding", original_route.binding_version);
    if (kind == VersionCase::resolver &&
        !e.status("v1 external effect occurs", world_.service.execute(world_.node.kernel()))) {
        return false;
    }
    if (!world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    world_.counter_profile = World::CounterProfile::v2;
    if (!world_.start() || !record_reader_versions(world_)) {
        return false;
    }
    if (kind == VersionCase::incompatible) {
        if (!world_.until("old prepared operation held for missing v1 Binding",
                          [&] { return task().hold_status == AYNI_UNSUPPORTED_VERSION; }) ||
            !world_.settle(8)) {
            return false;
        }
        const auto old = call("increment.v1");
        e.fact("matrix", "task_state", task().lifecycle);
        CallRouteEvidence route;
        return e.status("inspect old v1 semantic route",
                        inspect_call_route(world_.node.kernel(), old.logical_call_id, route)) &&
               e.equal("old capability version retained", route.capability_version, 1) &&
               e.equal("old binding version retained", route.binding_version, 1) &&
               e.equal("old operation retained under v2", old.operation_id,
                       original.operation_id) &&
               e.equal("v2 never executes v1 parameters", world_.service.submissions(), 0) &&
               e.equal("v2 never changes effects", world_.service.executions(), 0);
    }
    if (!world_.until("old v1 possible effect is unknown",
                      [&] { return call("increment.v1").phase == AYNI_CALL_RESULT_UNKNOWN; })) {
        return false;
    }
    auto query = [&] {
        const auto current = call("increment.v1");
        const ayni_call_query_result_v1 body{sizeof(body), 0, current.logical_call_id};
        world_.submit(AYNI_INTENT_CALL_QUERY_RESULT, &body, 0, current.query_revision);
    };
    query();
    if (!world_.until("v2 cannot query missing v1 semantics",
                      [&] {
                          const auto value = call("increment.v1");
                          return value.query_phase == AYNI_QUERY_FINISHED &&
                                 value.requires_seq <= world_.info().durable_seq;
                      }) ||
        !e.status("missing old query classified", call("increment.v1").query_status,
                  AYNI_UNSUPPORTED_VERSION) ||
        !world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    world_.counter_profile = World::CounterProfile::v2_with_v1_query;
    if (!world_.start() || !record_reader_versions(world_)) {
        return false;
    }
    query();
    if (!world_.until("old resolver receives original operation",
                      [&] { return world_.service.pending_queries() == 1; }) ||
        !e.status("old query returns original v1 effect",
                  world_.service.answer_query(AYNI_QUERY_RESULT_FOUND)) ||
        !world_.until("old operation reconciled", [&] {
            return call("increment.v1").query_conclusion == AYNI_QUERY_RESULT_FOUND;
        })) {
        return false;
    }
    e.fact("matrix", "query_conclusion", call("increment.v1").query_conclusion);
    return e.equal("resolver kept old operation", call("increment.v1").operation_id,
                   original.operation_id) &&
           e.equal("resolver cannot execute new effect", world_.service.executions(), 1) &&
           e.equal("resolver does not submit old parameters", world_.service.submissions(), 1);
}
// C-STORE-004 / C-VER-020,022: rejected input is compared byte-for-byte with
// its pre-boot image. This simulates unsupported readers, never an OTA binary.
bool Composite::storage_boundary(VersionFault fault, uint16_t version) {
    auto &e = world_.evidence;
    if (!analysis_pending()) {
        return false;
    }
    auto info = initialized<ayni_memory_info_v1>();
    char source[128]{};
    uint32_t size = 0;
    if (!e.status("source before storage boundary",
                  ayni_get_memory(world_.node.kernel(), world_.owner_session(), source_, &info,
                                  source, sizeof(source), &size))) {
        return false;
    }
    const ayni_memory_target_v1 target{sizeof(target), 0, source_};
    const auto deleted = world_.submit(AYNI_INTENT_MEMORY_DELETE, &target, 0, info.revision);
    if (!world_.until("deletion durable before unreadable boot",
                      [&] { return deleted.commit_seq <= world_.info().durable_seq; }) ||
        !e.equal("model count established before unreadable boot", task().model_calls_used, 1) ||
        !world_.crash(hosts::sim::CutAppend::lost) ||
        !replace_storage_version(world_, fault, version)) {
        return false;
    }
    const auto image = world_.node.simulation()->durable_storage();
    auto preserved = [&] {
        const auto &after = world_.node.simulation()->durable_storage();
        if (!e.equal("unreadable log count preserved", after.log.size(), image.log.size()) ||
            !e.equal("unreadable snapshots preserved", after.snapshots == image.snapshots, 1) ||
            !e.equal("unsupported reader installs no writer", after.writer_epoch,
                     image.writer_epoch)) {
            return false;
        }
        for (std::size_t i = 0; i < image.log.size(); ++i) {
            if (!e.equal("unsupported input bytes unchanged",
                         after.log[i].bytes == image.log[i].bytes &&
                             after.log[i].commit_seq == image.log[i].commit_seq &&
                             after.log[i].torn == image.log[i].torn,
                         1)) {
                return false;
            }
        }
        return true;
    };
    if (!world_.start(AYNI_LIFECYCLE_RESTRICTED) || !record_reader_versions(world_) ||
        !e.equal("unsupported schema diagnostic", world_.info().restriction,
                 AYNI_RESTRICTION_LOG_UNSUPPORTED) ||
        !world_.settle(4) || !preserved() || !world_.crash(hosts::sim::CutAppend::lost) ||
        !world_.start(AYNI_LIFECYCLE_RESTRICTED) || !preserved()) {
        return false;
    }
    e.fact("matrix", "kernel_lifecycle", world_.lifecycle());
    return e.equal("unreadable input never replays old effect", world_.service.executions(), 1) &&
           e.equal("unreadable input never sends new model", model_.pending(), 0) &&
           e.equal("unreadable input never notifies", notification_.submissions(), 0);
}
bool Composite::goal_version_boundary() {
    auto &e = world_.evidence;
    const auto original = task();
    if (!world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    goal_version_ = future_version;
    if (!world_.start() || !world_.until("missing old GoalType holds Task", [&] {
            return task().lifecycle == AYNI_TASK_PAUSED &&
                   task().hold_reason == AYNI_REASON_GOAL_TYPE_VERSION;
        })) {
        return false;
    }
    auto goal = initialized<ayni_goal_info_v1>();
    if (!e.status("retained old GoalType", ayni_get_goal(world_.node.kernel(), goal_, &goal)) ||
        !record_reader_versions(world_)) {
        return false;
    }
    e.fact("writer", "goal_type", goal.goal_type_version);
    e.fact("matrix", "task_state", task().lifecycle);
    return e.equal("old GoalType semantics never reset", goal.goal_type_version, 1) &&
           e.equal("missing GoalType keeps original Task", task().task_id, original.task_id) &&
           e.equal("new GoalType never replays old action", world_.service.executions(), 0) &&
           e.equal("new GoalType never opens capture", camera_.pending(), 0);
}
bool Composite::scene_version_boundary() {
    auto &e = world_.evidence;
    if (!world_.physical->prepare()) {
        return false;
    }
    auto motion = initialized<ayni_goal_create_v1>();
    motion.goal_type_id = text("turn");
    motion.goal_type_version = 1;
    motion.owner_instance_id = world_.instance(1);
    motion.parent_goal_id = parent_;
    motion.trigger = AYNI_TRIGGER_ONCE;
    world_.submit(AYNI_INTENT_GOAL_CREATE, &motion);
    if (!world_.until("v1 Scene issued actual permit",
                      [&] { return world_.physical->executor.samples() > 0; })) {
        return false;
    }
    const auto submissions = world_.physical->executor.submissions();
    auto before = initialized<ayni_scene_info_v1>();
    if (!e.status("saved v1 Scene", ayni_get_scene(world_.node.kernel(), text("look"), &before)) ||
        !world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    world_.physical->scene_version = future_version;
    if (!world_.start() || !world_.until("old physical lease stops independently", [&] {
            return world_.physical->info().phase == AYNI_CONTROL_STOPPED;
        })) {
        return false;
    }
    auto after = initialized<ayni_scene_info_v1>();
    if (!e.status("incompatible Scene remains diagnostic",
                  ayni_get_scene(world_.node.kernel(), text("look"), &after)) ||
        !e.equal("saved Scene version retained", after.saved_version, before.saved_version) ||
        !e.equal("new Scene version declared", after.registered_version, future_version) ||
        !e.equal("new Scene cannot consume old owner approval", after.consumable, 0) ||
        !e.equal("old Scene object retained", after.object_id, before.object_id)) {
        return false;
    }
    e.fact("writer", "scene", before.saved_version);
    e.fact("reader", "scene", after.registered_version);
    e.fact("matrix", "scene_consumable", after.consumable);
    world_.submit(AYNI_INTENT_GOAL_CREATE, &motion);
    if (!world_.settle(32)) {
        return false;
    }
    e.fact("matrix", "control_phase", world_.physical->info().phase);
    return e.equal("v2 Scene cannot resubmit old parameters",
                   world_.physical->executor.submissions(), submissions) &&
           e.equal("incompatible Scene remains independently stopped",
                   world_.physical->info().phase, AYNI_CONTROL_STOPPED);
}
bool Composite::compatible_current_profile() {
    auto &e = world_.evidence;
    if (!presentation_pending() || !finish()) {
        return false;
    }
    const auto original = task();
    TaskProgressEvidence before, after;
    if (!e.status("current profile checkpoint",
                  inspect_task_progress(world_.node.kernel(), original.task_id, before)) ||
        !world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    strategy_version_ = future_version;
    if (!world_.start() || !record_reader_versions(world_) || !world_.settle(16) ||
        !e.status("compatible reader checkpoint",
                  inspect_task_progress(world_.node.kernel(), original.task_id, after))) {
        return false;
    }
    e.fact("compat", "writer_version", before.writer_version);
    e.fact("compat", "reader_version", strategy_version_);
    e.fact("writer", "strategy", before.writer_version);
    e.fact("matrix", "task_state", task().lifecycle);
    return e.equal("declared compatible reader preserves exact checkpoint", after == before, 1) &&
           e.equal("compatible upgrade preserves completed Task", task().lifecycle,
                   AYNI_TASK_COMPLETED) &&
           e.equal("compatible upgrade preserves one action", world_.service.executions(), 1) &&
           e.equal("compatible upgrade preserves one notification", notification_.executions(), 1);
}
} // namespace ayni::runtime::experiments
