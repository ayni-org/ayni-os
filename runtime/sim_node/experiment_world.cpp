#include "experiment_support.h"
#include <algorithm>
#include <ayni/host_authority.h>
#include <ayni/runtime/common/default_config.h>
#include <cstring>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t catalog_capacity = 8;
constexpr uint32_t goal_capacity = 16;
constexpr uint32_t body_limit = 128;
constexpr uint32_t checkpoint_limit = 64;
constexpr uint64_t owner_id = 1;
ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 value{
        sizeof(value), 1, text("test.increment"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &value);
}
} // namespace
World::World(Evidence &out) : config(default_config()), evidence(out) {
    config.storage_buffer_bytes = 0;
    config.agent_capacity = 2;
    config.goal_type_capacity = catalog_capacity;
    config.goal_capacity = config.task_capacity = goal_capacity;
    config.goal_parameter_max_bytes = body_limit;
    config.task_checkpoint_max_bytes = checkpoint_limit;
    config.strategy_capacity = 2;
    config.call_capacity = operation_capacity;
    config.resource_request_capacity = operation_capacity;
    config.call_input_max_bytes = config.call_result_max_bytes = body_limit;
    config.call_capability_capacity = config.provider_capacity = config.binding_capacity =
        catalog_capacity;
}
World::~World() {
    if (node.kernel()) {
        // run() explicitly closes and reports its status. If an allocation throws
        // or close fails, this fallback must neither allocate nor post old borrows.
        // Abruptly discard only this entirely synchronous simulated deployment.
        service.quiesce();
        static_cast<void>(node.power_cut(hosts::sim::CutAppend::lost));
    }
}
ayni_status_t World::decide(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                            ayni_policy_output_v1 *output) {
    auto &self = *static_cast<World *>(ctx);
    if (self.policy) {
        return self.policy(*snapshot, *output);
    }
    auto result = initialized<ayni_task_call_result_v1>();
    const auto found = snapshot->call_result(snapshot->call_context, text("increment"), &result);
    if (found == AYNI_NOT_FOUND) {
        const ayni_call_request_v1 request{sizeof(request),
                                           AYNI_POLICY_REQUEST_CALL,
                                           1,
                                           0,
                                           text("increment"),
                                           text("increment"),
                                           {sizeof(ayni_scope_v1), 0, nullptr},
                                           {}};
        return output->request_call(output->call_context, &request);
    }
    if (found != AYNI_OK) {
        return found;
    }
    output->intents[0] = initialized<ayni_policy_intent_v1>();
    output->intents[0].kind = result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE;
    output->intents[0].expected_revision = snapshot->revision;
    output->intent_count = 1;
    return AYNI_OK;
}
bool World::register_profile() {
    const auto kernel = node.kernel();
    const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, owner_id, 1, 0};
    if (!evidence.status("register owner", ayni_register_principal(kernel, &owner))) {
        return false;
    }
    for (uint64_t id : {UINT64_C(1), UINT64_C(2)}) {
        const ayni_agent_desc_v1 agent{sizeof(agent), 0, id};
        if (!evidence.status("register agent", ayni_register_agent(kernel, &agent))) {
            return false;
        }
    }
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.strategy_id = text("experiment.policy");
    strategy.version = 1;
    strategy.ctx = this;
    strategy.decide = decide;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = checkpoint_limit;
    if (!evidence.status("register strategy", ayni_register_strategy(kernel, &strategy))) {
        return false;
    }
    auto goal = initialized<ayni_goal_type_v1>();
    goal.version = 1;
    goal.goal_type_id = text("action");
    goal.strategy_id = strategy.strategy_id;
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    goal.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    goal.export_requirements = requirements;
    if (!evidence.status("register action goal", ayni_register_goal_type(kernel, &goal))) {
        return false;
    }
    goal.goal_type_id = text("folder");
    goal.strategy_id = {};
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE);
    goal.export_requirements = nullptr;
    const ayni_capability_desc_v1 authority{
        sizeof(authority), 1, text("test.increment"), 0, 0, nullptr};
    const auto capability = service.capability();
    const auto provider = service.describe();
    const ayni_binding_v1 binding{sizeof(binding),
                                  1,
                                  text("increment"),
                                  capability.capability_id,
                                  1,
                                  1,
                                  provider.provider_id,
                                  provider.endpoint_id,
                                  {}};
    return evidence.status("register folder", ayni_register_goal_type(kernel, &goal)) &&
           evidence.status("register authority", ayni_register_capability(kernel, &authority)) &&
           evidence.status("register call", ayni_register_call_capability(kernel, &capability)) &&
           evidence.status("register provider", ayni_register_provider(kernel, &provider)) &&
           evidence.status("register binding", ayni_register_binding(kernel, &binding)) &&
           (!registration || evidence.status("register extension", registration(kernel)));
}
bool World::start() {
    if (!evidence.status("assemble", node.assemble(config)) || !register_profile() ||
        !evidence.status("start", ayni_start(node.kernel())) || !recover()) {
        return false;
    }
    if (lifecycle() == AYNI_LIFECYCLE_RUNNING) {
        const ayni_session_desc_v1 session{sizeof(session), AYNI_CHANNEL_MANAGEMENT, owner_id, 1,
                                           0};
        // C-AUTH-010: RUNNING precedes durable first-owner binding. Only this
        // documented readiness status permits another bounded local step.
        return until("session readiness", [&] {
            const auto status = ayni_open_session(node.kernel(), &session, &session_);
            if (status == AYNI_NOT_READY) {
                return false;
            }
            return evidence.status("open session", status);
        });
    }
    return true; // Recovery may deliberately end in diagnostic mode; the case must assert it.
}
bool World::recover() {
    return until("recovery bound", [&] { return lifecycle() != AYNI_LIFECYCLE_RECOVERING; });
}
bool World::step() {
    if (steps == step_limit) {
        return evidence.equal("total step bound", steps, step_limit - 1);
    }
    auto *host = node.simulation();
    evidence.context(steps, host->now_us());
    if (storage == Storage::complete) {
        evidence.fact("storage", "completed", host->complete_storage());
    }
    host->advance_us(tick_us);
    auto report = initialized<ayni_step_report_v1>();
    ++steps;
    evidence.context(steps, host->now_us());
    const bool valid =
        evidence.status("step", ayni_step(node.kernel(), step_items, step_time_us, &report));
    evidence.fact("step", "count", steps);
    evidence.fact("storage", "durable_seq", info().durable_seq);
    evidence.changed("boot_epoch", info().boot_epoch);
    evidence.fact("external", "executions", service.executions());
    return valid && observe();
}
bool World::observe() {
    for (const auto kind : {AYNI_OBJECT_GOAL, AYNI_OBJECT_TASK, AYNI_OBJECT_CALL}) {
        uint64_t ids[goal_capacity]{};
        uint32_t size = 0;
        if (!evidence.status("observe objects",
                             ayni_list_objects(node.kernel(), kind, ids, goal_capacity, &size))) {
            return false;
        }
        for (uint32_t at = 0; at < size; ++at) {
            const auto key = std::to_string(kind) + ":" + std::to_string(ids[at]);
            if (kind == AYNI_OBJECT_TASK) {
                auto out = initialized<ayni_task_info_v1>();
                if (!evidence.status("observe task", ayni_get_task(node.kernel(), ids[at], &out))) {
                    return false;
                }
                evidence.changed(key + ":state", out.lifecycle);
                evidence.changed(key + ":hold_reason", out.hold_reason);
                evidence.changed_status(key + ":hold_status", out.hold_status);
                evidence.changed(key + ":goal", out.goal_id);
                evidence.changed(key + ":owner", out.owner_instance_id);
                evidence.changed(key + ":commit_seq", out.commit_seq);
                evidence.changed(key + ":steps", out.step_count);
            } else if (kind == AYNI_OBJECT_CALL) {
                auto out = initialized<ayni_call_info_v1>();
                if (!evidence.status("observe call", ayni_get_call(node.kernel(), ids[at], &out))) {
                    return false;
                }
                evidence.changed(key + ":phase", out.phase);
                evidence.changed(key + ":operation", out.operation_id);
                evidence.changed(key + ":task", out.task_id);
                evidence.changed(key + ":outcome", out.outcome);
                evidence.changed(key + ":effect", out.effect);
                evidence.changed(key + ":requires_seq", out.requires_seq);
                evidence.changed(key + ":request", out.attempt.request_id);
                evidence.changed(key + ":boot", out.attempt.boot_epoch);
            } else {
                auto out = initialized<ayni_goal_info_v1>();
                if (!evidence.status("observe goal", ayni_get_goal(node.kernel(), ids[at], &out))) {
                    return false;
                }
                evidence.changed(key + ":state", out.lifecycle);
                evidence.changed(key + ":commit_seq", out.commit_seq);
                evidence.changed(key + ":transition", out.transition_id);
            }
        }
    }
    return evidence.ok();
}
bool World::settle(uint32_t rounds) {
    for (uint32_t index = 0; index < rounds && evidence.ok(); ++index) {
        if (!step()) {
            return false;
        }
    }
    return evidence.ok();
}
bool World::until(const char *name, const std::function<bool()> &condition, uint32_t bound) {
    for (uint32_t index = 0; index < bound && evidence.ok(); ++index) {
        if (condition()) {
            return true;
        }
        if (!step()) {
            return false;
        }
    }
    return evidence.equal(name, condition() ? 1 : 0, 1);
}
bool World::crash(hosts::sim::CutAppend append) {
    evidence.fact("fault", "pending_storage", node.simulation()->pending_storage());
    evidence.fact("fault", "append_disposition", static_cast<uint64_t>(append));
    evidence.fact("fault", "external_executions", service.executions());
    // No asynchronous threads exist in this deployment. Drop every client pointer,
    // then Host::power_cut ends storage borrows before SimNode releases the arena.
    service.quiesce();
    session_ = 0;
    return evidence.status("power cut", node.power_cut(append));
}
ayni_status_t World::close() {
    if (!node.kernel()) {
        return AYNI_OK;
    }
    if (lifecycle() != AYNI_LIFECYCLE_CONFIGURING && lifecycle() != AYNI_LIFECYCLE_STOPPED) {
        const auto begin = ayni_begin_shutdown(node.kernel());
        if (!evidence.status("begin shutdown", begin)) {
            return begin;
        }
        for (uint32_t index = 0; index < step_limit; ++index) {
            while (service.pending()) {
                const auto receipt = service.receipt(AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN);
                if (!evidence.status("drain call", receipt)) {
                    return receipt;
                }
            }
            const auto query = service.answer_query(AYNI_QUERY_RESULT_UNKNOWN);
            if (query != AYNI_NOT_FOUND && !evidence.status("drain query", query)) {
                return query;
            }
            if (lifecycle() == AYNI_LIFECYCLE_STOPPED) {
                break;
            }
            // Cleanup remains possible after a failed case. It never clears the first
            // failure, and its status is reported independently by the runner.
            node.simulation()->complete_storage();
            node.simulation()->advance_us(tick_us);
            auto report = initialized<ayni_step_report_v1>();
            const auto advanced = ayni_step(node.kernel(), step_items, step_time_us, &report);
            if (!evidence.status("shutdown step", advanced)) {
                return advanced;
            }
        }
    }
    const auto released = node.release();
    evidence.status("destroy", released);
    return released;
}
ayni_intent_result_v1 World::submit(ayni_intent_kind_t kind, const void *body, uint64_t command,
                                    uint32_t revision, ayni_status_t expected) {
    auto intent = initialized<ayni_intent_v1>();
    intent.kind = kind;
    intent.session_id = session_;
    intent.command_id = command ? command : ++next_command_;
    next_command_ = std::max(next_command_, intent.command_id);
    intent.expected_revision = revision;
    intent.body = body;
    auto result = initialized<ayni_intent_result_v1>();
    auto error = initialized<ayni_error_v1>();
    evidence.status("submit", ayni_submit_intent(node.kernel(), &intent, &result, &error),
                    expected);
    evidence.fact("command", "id", intent.command_id);
    evidence.fact("command", "commit_seq", result.commit_seq);
    return result;
}
uint64_t World::create(const char *type, uint64_t command, uint64_t agent) {
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_version = 1;
    body.goal_type_id = text(type);
    body.owner_instance_id = instance(agent);
    body.trigger = std::strcmp(type, "folder") == 0 ? AYNI_TRIGGER_NONE : AYNI_TRIGGER_ONCE;
    return submit(AYNI_INTENT_GOAL_CREATE, &body, command).object.id;
}
bool World::grant(uint64_t agent) {
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     1,
                                     text("test.increment"),
                                     instance(agent),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 0, nullptr}};
    submit(AYNI_INTENT_GRANT_CREATE, &grant);
    return settle();
}
bool World::request_query() {
    const auto current = call();
    const ayni_call_query_result_v1 body{sizeof(body), 0, current.logical_call_id};
    submit(AYNI_INTENT_CALL_QUERY_RESULT, &body, 0, current.query_revision);
    return evidence.ok() && until("query dispatch", [&] { return service.pending_queries() != 0; });
}
ayni_call_info_v1 World::call() const {
    uint64_t ids[operation_capacity]{};
    uint32_t count = 0;
    auto out = initialized<ayni_call_info_v1>();
    if (evidence.status("list calls", ayni_list_objects(node.kernel(), AYNI_OBJECT_CALL, ids,
                                                        operation_capacity, &count)) &&
        count) {
        evidence.status("get call", ayni_get_call(node.kernel(), ids[count - 1], &out));
    }
    return out;
}
ayni_kernel_info_v1 World::info() const {
    auto out = initialized<ayni_kernel_info_v1>();
    evidence.status("get info", ayni_get_kernel_info(node.kernel(), &out));
    return out;
}
ayni_lifecycle_t World::lifecycle() const {
    ayni_lifecycle_t out = 0;
    evidence.status("get lifecycle", ayni_get_lifecycle(node.kernel(), &out));
    return out;
}
uint32_t World::count(ayni_object_kind_t kind) const {
    uint32_t out = 0;
    const auto status = ayni_list_objects(node.kernel(), kind, nullptr, 0, &out);
    evidence.status("count objects", status, out ? AYNI_BUFFER_TOO_SMALL : AYNI_OK);
    return out;
}
uint64_t World::instance(uint64_t agent) const {
    auto out = initialized<ayni_agent_info_v1>();
    evidence.status("get instance", ayni_get_agent(node.kernel(), agent, &out));
    return out.instance_id;
}
uint64_t World::metric(Metric metric) const {
    switch (metric) {
    case Metric::executions:
        return service.executions();
    case Metric::pending_calls:
        return service.pending();
    case Metric::pending_storage:
        return node.simulation()->pending_storage();
    case Metric::goal_count:
        return count(AYNI_OBJECT_GOAL);
    case Metric::call_count:
        return count(AYNI_OBJECT_CALL);
    case Metric::call_phase:
        return call().phase;
    case Metric::call_outcome:
        return call().outcome;
    case Metric::call_effect:
        return call().effect;
    case Metric::lifecycle:
        return lifecycle();
    case Metric::writer_epoch:
        return node.simulation()->writer_epoch();
    case Metric::faults_applied:
        return node.simulation()->faults_applied();
    }
    evidence.status("unknown metric", AYNI_INVALID_ARGUMENT);
    return 0;
}
} // namespace ayni::runtime::experiments
