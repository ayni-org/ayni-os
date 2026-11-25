#include "experiment_composite.h"
#include "experiment_execution.h"
#include "experiment_replacement.h"
#include "experiment_shared.h"
#include "experiment_support.h"
#include <algorithm>
#include <ayni/host_authority.h>
#include <ayni/memory.h>
#include <ayni/runtime/common/default_config.h>
#include <cstring>
#include <vector>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t catalog_capacity = 8;
constexpr uint32_t goal_capacity = 16;
constexpr uint32_t body_limit = 128;
constexpr uint32_t checkpoint_limit = 64;
constexpr uint32_t command_flush_steps = 16;
constexpr uint64_t owner_id = 1;
ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 value{
        sizeof(value), 1, text("test.increment"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &value);
}
ayni_status_t request_increment(ayni_policy_output_v1 &output) {
    ValueBuilder<1> input;
    input.text(0, {});
    ayni_call_request_v1 request{sizeof(request),
                                 AYNI_POLICY_REQUEST_CALL,
                                 1,
                                 0,
                                 text("increment"),
                                 text("increment"),
                                 {},
                                 AYNI_CALL_MODE_FOREGROUND,
                                 0};
    const auto built = input.finish(request.value);
    if (built != AYNI_OK) {
        return built;
    }
    return output.request_call(output.call_context, &request);
}
ayni_status_t report_action(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output,
                            const ayni_task_call_result_v1 &result) {
    output.intents[0] = initialized<ayni_policy_intent_v1>();
    output.intents[0].kind = result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                   : AYNI_POLICY_REPORT_FAILURE;
    output.intents[0].expected_revision = snapshot.revision;
    output.intent_count = 1;
    return AYNI_OK;
}
ayni_status_t decide_action(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
    auto result = initialized<ayni_task_call_result_v1>();
    const auto found = snapshot.call_result(snapshot.call_context, text("increment"), &result);
    if (found == AYNI_NOT_FOUND) {
        return request_increment(output);
    }
    return found == AYNI_OK ? report_action(snapshot, output, result) : found;
}
} // namespace
World::World(Evidence &out, ayni_call_query_authority_t query, Deployment deployment)
    : config(default_config()), service(query), evidence(out) {
    config.storage_buffer_bytes = 0;
    config.agent_capacity = 2;
    config.goal_type_capacity = catalog_capacity;
    config.goal_capacity = config.task_capacity = goal_capacity;
    config.goal_parameter_max_bytes = body_limit;
    config.task_checkpoint_max_bytes = checkpoint_limit;
    config.strategy_capacity = 2;
    config.call_capacity = operation_capacity;
    config.resource_request_capacity = operation_capacity;
    // The Call strides fit the contracts the deployment registers.
    std::vector<CallBytes> contracts{call_bytes(service.capability().contract)};
    const CallBytes model{fixtures::ModelProbe::call_input_bytes(),
                          fixtures::ModelProbe::call_result_bytes};
    const CallBytes camera{providers::mock::subscription_input_bytes(),
                           providers::mock::subscription_result_bytes};
    const CallBytes notification{text_value_bytes(ActionService::notification_text_bytes), 0};
    config.call_capability_capacity = config.provider_capacity = config.binding_capacity =
        catalog_capacity;
    if (deployment == Deployment::replacement) {
        config.resource_capacity = 1;
        config.task_checkpoint_max_bytes = config.goal_parameter_max_bytes =
            strategies::ToolLoop::checkpoint_bytes;
        contracts.push_back(model);
        config.strategy_capacity = 2;
        config.max_turns = 2;
        replacement = std::make_unique<Replacement>(*this);
    }
    if (deployment == Deployment::physical || deployment == Deployment::shared_physical ||
        deployment == Deployment::composite) {
        config.resource_capacity = config.scene_capacity = config.observation_source_capacity = 1;
        config.executor_capacity = config.execution_binding_capacity = 1;
        physical = std::make_unique<Physical>(*this);
        physical->configure();
        contracts.push_back(physical->call_bytes());
    }
    if (deployment == Deployment::shared || deployment == Deployment::shared_physical) {
        config.goal_type_capacity = 24;
        config.strategy_capacity = 5;
        config.goal_parameter_max_bytes = strategies::ToolLoop::checkpoint_bytes;
        config.task_checkpoint_max_bytes = strategies::ToolLoop::checkpoint_bytes;
        config.resource_capacity = 5;
        config.observation_source_capacity = 1;
        contracts.insert(contracts.end(), {model, camera, notification});
        config.max_turns = 2;
        if (physical) {
            config.resource_capacity = 6;
            config.strategy_capacity = 6;
            config.observation_source_capacity = 2;
        }
        shared = std::make_unique<SharedServices>(*this);
    }
    if (deployment == Deployment::composite) {
        config.resource_capacity = 4;
        config.strategy_capacity = 3;
        config.goal_requirement_capacity = 5;
        config.observation_source_capacity = 2;
        contracts.insert(contracts.end(), {model, camera, notification});
        config.max_turns = 2;
        composite = std::make_unique<Composite>(*this);
    }
    fit_call_bytes(config, contracts);
}
World::~World() {
    if (node.kernel()) {
        // run() explicitly closes and reports its status. If an allocation throws
        // or close fails, this fallback must neither allocate nor post old borrows.
        // Abruptly discard only this entirely synchronous simulated deployment.
        detach_clients();
        static_cast<void>(node.power_cut(hosts::sim::CutAppend::lost));
    }
    if (shared) {
        shared->abandon_kernel();
        static_cast<void>(shared->close());
    }
    if (replacement) {
        replacement->abandon_kernel();
        static_cast<void>(replacement->close());
    }
    if (composite) {
        composite->abandon_kernel();
        static_cast<void>(composite->close());
    }
    if (physical) {
        // Also needed when a legal script ended immediately after Kernel loss.
        physical->executor.abandon_kernel();
        static_cast<void>(physical->executor.power_cycle());
        static_cast<void>(physical->executor.close());
    }
}
ayni_status_t World::decide(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                            ayni_policy_output_v1 *output) {
    auto &self = *static_cast<World *>(ctx);
    if (self.policy) {
        return self.policy(*snapshot, *output);
    }
    return decide_action(*snapshot, *output);
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
    constexpr uint32_t future_counter_version = 2;
    const auto counter_version =
        counter_profile == CounterProfile::v1 ? 1U : future_counter_version;
    auto capability = service.capability();
    capability.version = counter_version;
    auto provider = service.describe();
    provider.version = counter_version;
    provider.capabilities = &capability;
    const ayni_binding_v1 binding{sizeof(binding),
                                  counter_version,
                                  text("increment"),
                                  capability.capability_id,
                                  counter_version,
                                  counter_version,
                                  provider.provider_id,
                                  provider.endpoint_id,
                                  {},
                                  0,
                                  0,
                                  nullptr};
    if (counter_profile == CounterProfile::v2_with_v1_query) {
        auto old_capability = service.capability();
        auto resolver = service.describe();
        resolver.capabilities = &old_capability;
        // A resolver is allowed to read old operation outcomes, never to dispatch.
        resolver.submit = [](void *, const ayni_provider_request_v1 *,
                             ayni_provider_completion_t *) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        };
        if (!evidence.status("register v1 query resolver",
                             ayni_register_provider(kernel, &resolver))) {
            return false;
        }
    }
    if (counter_version != authority.version) {
        auto current = authority;
        current.version = counter_version;
        if (!evidence.status("register current counter authority",
                             ayni_register_capability(kernel, &current))) {
            return false;
        }
    }
    return evidence.status("register folder", ayni_register_goal_type(kernel, &goal)) &&
           evidence.status("register authority", ayni_register_capability(kernel, &authority)) &&
           evidence.status("register call", ayni_register_call_capability(kernel, &capability)) &&
           evidence.status("register provider", ayni_register_provider(kernel, &provider)) &&
           evidence.status("register binding", ayni_register_binding(kernel, &binding)) &&
           (!physical || physical->register_profile(kernel)) &&
           (!shared || shared->register_profile(kernel)) &&
           (!replacement || replacement->register_profile(kernel)) &&
           (!composite || composite->register_profile(kernel)) &&
           (!registration || evidence.status("register extension", registration(kernel)));
}
// This finite policy consumes the original call exactly once, then commits a
// derived summary, machine step and private checkpoint in one owner transition.
bool World::application_policy() {
    policy = [](const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        if (snapshot.checkpoint.state.size != 0) {
            return decide_action(snapshot, output);
        }
        auto result = initialized<ayni_task_call_result_v1>();
        const auto found = snapshot.call_result(snapshot.call_context, text("increment"), &result);
        if (found == AYNI_NOT_FOUND) {
            return request_increment(output);
        }
        if (found != AYNI_OK) {
            return found;
        }
        if (result.outcome != AYNI_CALL_COMPLETED) {
            return report_action(snapshot, output, result);
        }
        if (!snapshot.memory || output.checkpoint_capacity == 0) {
            return AYNI_UNSUPPORTED;
        }
        auto proposal = initialized<ayni_memory_proposal_v1>();
        proposal.source = AYNI_MEMORY_DERIVED;
        proposal.step_key = text("summary");
        proposal.body = text("action completed");
        const auto proposed = snapshot.memory->propose(snapshot.memory->context, &proposal);
        if (proposed != AYNI_OK) {
            return proposed;
        }
        auto &intent = output.intents[0];
        intent = initialized<ayni_policy_intent_v1>();
        intent.kind = AYNI_POLICY_WRITE_MEMORY;
        intent.expected_revision = snapshot.revision;
        intent.step_key_size = proposal.step_key.size;
        std::memcpy(intent.step_key, proposal.step_key.data, proposal.step_key.size);
        output.intent_count = 1;
        output.checkpoint[0] = 1;
        output.checkpoint_size = 1;
        return AYNI_OK;
    };
    return true;
}
bool World::start_recovery() {
    schedule_.reset();
    storage = Storage::complete;
    // Core releases the bodies the deployment's Providers and ToolLoop store (ADR-0096).
    const ayni_body_store_v1 *bodies = shared        ? shared->bodies()
                                       : replacement ? replacement->bodies()
                                                     : nullptr;
    return evidence.status("assemble", node.assemble(config, bodies)) && register_profile() &&
           evidence.status("start", ayni_start(node.kernel()));
}
bool World::start(ayni_lifecycle_t expected) {
    return start_recovery() && finish_recovery(expected);
}
bool World::finish_recovery(ayni_lifecycle_t expected) {
    if (!recover()) {
        return false;
    }
    const auto recovered = lifecycle();
    evidence.fact("recovery", "restriction", info().restriction);
    if (!evidence.equal("recovery lifecycle", recovered, expected)) {
        return false;
    }
    if (recovered != AYNI_LIFECYCLE_RUNNING) {
        return true; // A diagnostic result is permitted only by this explicit case expectation.
    }
    const ayni_session_desc_v1 session{sizeof(session), AYNI_CHANNEL_MANAGEMENT, owner_id, 1, 0};
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
bool World::recover() {
    return until("recovery bound", [&] { return lifecycle() != AYNI_LIFECYCLE_RECOVERING; });
}
bool World::step() {
    return advance_step(
        shared ? (schedule_ ? shared->clock_delta(schedule_->report, schedule_->seen_wakes) : 0)
               : tick_us);
}
bool World::step_now() {
    return advance_step(0);
}
bool World::advance_step(uint64_t clock_delta_us) {
    if (steps == step_limit) {
        return evidence.equal("total step bound", steps, step_limit - 1);
    }
    auto *host = node.simulation();
    evidence.context(steps, host->now_us());
    if (storage == Storage::complete) {
        evidence.fact("storage", "completed", host->complete_storage());
    }
    node.advance_clock(clock_delta_us);
    evidence.context(steps, host->now_us());
    if (shared && !shared->advance()) {
        return false;
    }
    if (replacement && !replacement->advance()) {
        return false;
    }
    if (composite && !composite->advance()) {
        return false;
    }
    if (physical && !physical->advance()) {
        return false;
    }
    auto report = initialized<ayni_step_report_v1>();
    ++steps;
    evidence.context(steps, host->now_us());
    const auto seen_wakes = host->wake_count();
    const bool valid =
        evidence.status("step", ayni_step(node.kernel(), items_per_step, step_time_us, &report));
    schedule_ = HostSchedule{report, seen_wakes};
    evidence.fact("step", "count", steps);
    evidence.fact("step", "item_budget", items_per_step);
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
                evidence.changed(key + ":query_boot", out.query_attempt.boot_epoch);
                evidence.changed(key + ":query_request", out.query_attempt.request_id);
                evidence.changed(key + ":query_phase", out.query_phase);
                evidence.changed(key + ":query_conclusion", out.query_conclusion);
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
bool World::await_prepared(const std::function<ayni_call_info_v1()> &get_call) {
    // ADR-0104 permits DISPATCHING to queue before PREPARED is durable. Keep the
    // existing restart fixtures at their intended durable PREPARED boundary by
    // making each append immediately eligible; its completion consumes the sole
    // item in the following step, before another Call transition can run.
    items_per_step = 1;
    const auto interval = config.storage_write_interval_us ? config.storage_write_interval_us
                                                           : AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
    for (uint32_t at = 0; at < step_limit && evidence.ok(); ++at) {
        const auto current = get_call();
        if (current.phase == AYNI_CALL_PREPARED && current.requires_seq &&
            current.requires_seq <= info().durable_seq) {
            return evidence.equal("prepared before provider submission", service.submissions(), 0);
        }
        node.advance_clock(interval);
        if (!step()) {
            return false;
        }
    }
    return evidence.equal("durable prepared boundary reached", 0, 1);
}
bool World::crash(hosts::sim::CutAppend append) {
    evidence.fact("fault", "pending_storage", node.simulation()->pending_storage());
    evidence.fact("fault", "append_disposition", static_cast<uint64_t>(append));
    evidence.fact("fault", "external_executions", service.executions());
    // No asynchronous threads exist in this deployment. Drop every client pointer,
    // then Host::power_cut ends storage borrows before SimNode releases the arena.
    detach_clients();
    return evidence.status("power cut", node.power_cut(append));
}
void World::detach_clients() {
    service.quiesce();
    if (shared) {
        shared->abandon_kernel();
    }
    if (replacement) {
        replacement->abandon_kernel();
    }
    if (composite) {
        composite->abandon_kernel();
    }
    if (physical) {
        physical->executor.abandon_kernel();
    }
    session_ = 0;
}
bool World::fault_pending(hosts::sim::StorageFault fault) {
    const auto pending = node.simulation()->pending_append();
    if (!pending) {
        return evidence.status("fault target must be pending", AYNI_NOT_FOUND);
    }
    evidence.fact("fault", "append_commit_seq", pending->commit_seq);
    evidence.fact("fault", "append_outcome", static_cast<uint64_t>(fault));
    return evidence.status("fault exact append",
                           node.simulation()->fault_pending_append(pending->commit_seq, fault));
}
bool World::complete_storage() {
    evidence.fact("storage", "explicit_completions", node.simulation()->complete_storage());
    return evidence.ok();
}
bool World::complete_old_writer() {
    auto &host = *node.simulation();
    const auto before = host.log().size();
    if (!evidence.equal("late writer copy actually exists", host.pending_late_storage() != 0, 1)) {
        return false;
    }
    return evidence.equal("old writer rejected after fence", host.complete_late_storage(), 0) &&
           evidence.equal("late writer cannot change log", host.log().size(), before);
}
ayni_status_t World::close() {
    if (replacement) {
        replacement->begin_close();
    }
    if (composite) {
        composite->begin_close();
    }
    if (shared && !shared->begin_close()) {
        return evidence.error_status();
    }
    if (!node.kernel()) {
        return close_services();
    }
    if (lifecycle() != AYNI_LIFECYCLE_CONFIGURING && lifecycle() != AYNI_LIFECYCLE_STOPPED) {
        const auto accepted_before_shutdown = service.submissions();
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
            node.advance_clock(tick_us);
            evidence.context(steps, node.simulation()->now_us());
            if (shared && !shared->advance()) {
                return AYNI_INVALID_STATE;
            }
            if (replacement && !replacement->advance()) {
                return AYNI_INVALID_STATE;
            }
            if (composite && !composite->advance()) {
                return AYNI_INVALID_STATE;
            }
            if (physical && !physical->advance()) {
                return AYNI_INVALID_STATE;
            }
            auto report = initialized<ayni_step_report_v1>();
            const auto advanced = ayni_step(node.kernel(), step_items, step_time_us, &report);
            if (!evidence.status("shutdown step", advanced)) {
                return advanced;
            }
            if (!evidence.equal("shutdown creates no delivery", service.submissions(),
                                accepted_before_shutdown)) {
                return AYNI_INVALID_STATE;
            }
        }
    }
    if (lifecycle() != AYNI_LIFECYCLE_CONFIGURING && lifecycle() != AYNI_LIFECYCLE_STOPPED) {
        observe(); // Preserve final public state for a bounded-shutdown failure.
        evidence.status("shutdown bound", AYNI_BUSY);
        return AYNI_BUSY;
    }
    const auto released = node.release();
    evidence.status("destroy", released);
    return released == AYNI_OK ? close_services() : released;
}
ayni_status_t World::close_services() {
    const auto composed = composite ? composite->close() : AYNI_OK;
    evidence.status("close composite services", composed);
    const auto replaced = replacement ? replacement->close() : AYNI_OK;
    evidence.status("close replacement services", replaced);
    const auto services = shared ? shared->close() : AYNI_OK;
    evidence.status("close shared services", services);
    const auto execution = physical ? physical->close() : AYNI_OK;
    evidence.status("close independent executor", execution);
    return composed != AYNI_OK   ? composed
           : replaced != AYNI_OK ? replaced
           : services != AYNI_OK ? services
                                 : execution;
}
ayni_intent_result_v1 World::submit(ayni_intent_kind_t kind, const void *body, uint64_t command,
                                    uint32_t revision, ayni_status_t expected,
                                    std::optional<ayni_reason_t> reason) {
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
    evidence.fact("command", "error_reason", error.reason);
    if (reason) {
        evidence.equal("intent error reason", error.reason, *reason);
    }
    evidence.fact("command", "id", intent.command_id);
    evidence.fact("command", "commit_seq", result.commit_seq);
    return result;
}
ayni_goal_create_v1 World::goal_body(const char *type, uint64_t agent) const {
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_version = 1;
    body.goal_type_id = text(type);
    body.owner_instance_id = instance(agent);
    body.trigger = std::strcmp(type, "folder") == 0 ? AYNI_TRIGGER_NONE : AYNI_TRIGGER_ONCE;
    return body;
}
bool World::exhaust_storage() {
    auto &host = *node.simulation();
    host.set_storage_capacity(0);
    const auto ports = host.table();
    uint64_t available = UINT64_MAX;
    if (!evidence.status("actual storage free bytes",
                         ports.storage_free_bytes(ports.ctx, &available)) ||
        !evidence.equal("full medium reports no space", available, 0)) {
        return false;
    }
    const auto body = goal_body(physical ? "turn" : "action", 1);
    submit(AYNI_INTENT_GOAL_CREATE, &body, 0, 0, AYNI_STORAGE_FAILED, AYNI_REASON_STORAGE_SPACE);
    return evidence.ok();
}
void World::restore_storage() {
    node.simulation()->set_storage_capacity(UINT64_MAX);
}
uint64_t World::create(const char *type, uint64_t command, uint64_t agent) {
    const auto body = goal_body(type, agent);
    const auto result = submit(AYNI_INTENT_GOAL_CREATE, &body, command);
    if (evidence.ok()) {
        accepted_goal_ = AcceptedGoal{type, body, command ? command : next_command_, result};
        accepted_goal_->body.goal_type_id = text(accepted_goal_->type.c_str());
    }
    return result.object.id;
}
bool World::grant(uint64_t agent, const char *capability) {
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     1,
                                     text(capability),
                                     instance(agent),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 0, nullptr}};
    const auto result = submit(AYNI_INTENT_GRANT_CREATE, &grant);
    if (evidence.ok()) {
        grant_ = result.object.id;
    }
    return shared ? until("Grant durable", [&] { return result.commit_seq <= info().durable_seq; })
                  : settle();
}
bool World::revoke() {
    if (!grant_) {
        return evidence.status("grant required", AYNI_NOT_FOUND);
    }
    return revoke(*grant_);
}
bool World::revoke(uint64_t grant_id) {
    auto current = initialized<ayni_grant_info_v1>();
    if (!evidence.status("get grant", ayni_get_grant(node.kernel(), grant_id, &current))) {
        return false;
    }
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grant_id};
    submit(AYNI_INTENT_GRANT_REVOKE, &body, 0, current.revision);
    return evidence.ok();
}
bool World::save_call() {
    const auto current = call();
    if (!evidence.equal("save nonzero call", current.logical_call_id != 0, 1)) {
        return false;
    }
    const auto kernel = info();
    saved_call_ = SavedCall{current, node.simulation()->writer_epoch(), kernel.boot_epoch,
                            kernel.durable_seq};
    evidence.fact("snapshot", "saved_writer_epoch", saved_call_->writer_epoch);
    evidence.fact("snapshot", "saved_durable_seq", saved_call_->durable_seq);
    return true;
}
bool World::same_call() {
    if (!saved_call_) {
        return evidence.status("saved call required", AYNI_NOT_FOUND);
    }
    auto current = initialized<ayni_call_info_v1>();
    const auto &saved = saved_call_->call;
    return evidence.status("find saved call",
                           ayni_get_call(node.kernel(), saved.logical_call_id, &current)) &&
           evidence.equal("logical call preserved", current.logical_call_id,
                          saved.logical_call_id) &&
           evidence.equal("operation preserved", current.operation_id, saved.operation_id) &&
           evidence.equal("task preserved", current.task_id, saved.task_id) &&
           evidence.equal("owner preserved", current.owner_instance_id, saved.owner_instance_id);
}
bool World::saved_call_missing() {
    if (!saved_call_) {
        return evidence.status("saved call required", AYNI_NOT_FOUND);
    }
    auto current = initialized<ayni_call_info_v1>();
    return evidence.status(
        "uncommitted call absent",
        ayni_get_call(node.kernel(), saved_call_->call.logical_call_id, &current), AYNI_NOT_FOUND);
}
bool World::retry(ayni_status_t expected) {
    if (!accepted_goal_) {
        return evidence.status("accepted goal required", AYNI_NOT_FOUND);
    }
    const auto &original = *accepted_goal_;
    const auto repeated =
        submit(AYNI_INTENT_GOAL_CREATE, &original.body, original.command, 0, expected);
    if (expected != AYNI_OK) {
        return evidence.ok();
    }
    return evidence.equal("same command goal", repeated.object.id, original.result.object.id) &&
           evidence.equal("same command sequence", repeated.commit_seq, original.result.commit_seq);
}
bool World::request_query(ayni_status_t expected) {
    const auto current = call();
    const ayni_call_query_result_v1 body{sizeof(body), 0, current.logical_call_id};
    submit(AYNI_INTENT_CALL_QUERY_RESULT, &body, 0, current.query_revision, expected);
    return evidence.ok() && (expected != AYNI_OK || until("query dispatch", [&] {
                                 return service.pending_queries() != 0;
                             }));
}
bool World::conflict_command() {
    if (!accepted_goal_) {
        return evidence.status("accepted goal required", AYNI_NOT_FOUND);
    }
    auto changed = accepted_goal_->body;
    const uint8_t different = 1;
    changed.parameters = {&different, sizeof(different)};
    submit(AYNI_INTENT_GOAL_CREATE, &changed, accepted_goal_->command, 0,
           AYNI_IDEMPOTENCY_CONFLICT);
    return evidence.ok();
}
bool World::expire_command() {
    if (!accepted_goal_) {
        return evidence.status("accepted goal required", AYNI_NOT_FOUND);
    }
    // Fill the real bounded retention table; a numeric jump alone cannot evict
    // a retained command. No test-only expiry rule is introduced (C-WIRE-005).
    auto original = *accepted_goal_;
    original.body.goal_type_id = text(original.type.c_str());
    create("folder", original.command + config.command_capacity + 1);
    if (!settle()) {
        return false;
    }
    for (uint32_t at = 0; at <= config.command_capacity; ++at) {
        auto current = initialized<ayni_goal_info_v1>();
        if (!evidence.status("get original goal",
                             ayni_get_goal(node.kernel(), original.result.object.id, &current))) {
            return false;
        }
        const ayni_goal_target_v1 target{sizeof(target), 0, current.goal_id};
        const auto kind = current.lifecycle == AYNI_GOAL_PAUSED ? AYNI_INTENT_GOAL_RESUME
                                                                : AYNI_INTENT_GOAL_PAUSE;
        submit(kind, &target, 0, current.revision);
        if (!settle(command_flush_steps)) {
            return false;
        }
    }
    submit(AYNI_INTENT_GOAL_CREATE, &original.body, original.command, 0, AYNI_EXPIRED);
    return evidence.ok();
}
bool World::read_result() {
    const auto current = call();
    uint32_t bytes = UINT32_MAX;
    return evidence.status(
               "copy durable result",
               ayni_copy_call_result(node.kernel(), current.logical_call_id, nullptr, 0, &bytes)) &&
           evidence.equal("empty counter result", bytes, 0);
}
bool World::cancel_goal() {
    if (!accepted_goal_) {
        return evidence.status("accepted goal required", AYNI_NOT_FOUND);
    }
    auto current = initialized<ayni_goal_info_v1>();
    if (!evidence.status(
            "get goal", ayni_get_goal(node.kernel(), accepted_goal_->result.object.id, &current))) {
        return false;
    }
    const ayni_goal_target_v1 body{sizeof(body), 0, current.goal_id};
    submit(AYNI_INTENT_GOAL_CANCEL, &body, 0, current.revision);
    return evidence.ok();
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
    case Metric::replacement_completed:
        return replacement ? replacement->completed() : 0;
    case Metric::composite_completed:
        return composite ? composite->completed() : 0;
    case Metric::shared_completed:
        return shared ? shared->completed() : 0;
    case Metric::physical_submissions:
        return physical ? physical->executor.submissions() : 0;
    case Metric::physical_samples:
        return physical ? physical->executor.samples() : 0;
    case Metric::physical_challenges:
        return physical ? physical->executor.challenges() : 0;
    case Metric::physical_renewals:
        return physical ? physical->executor.renewals() : 0;
    case Metric::held_renewals:
        return physical ? physical->executor.held(AYNI_EXECUTOR_RENEW) : 0;
    case Metric::control_phase:
        return physical ? physical->info().phase : 0;
    case Metric::control_deadline:
        return physical ? physical->deadline_offset() : 0;
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
    case Metric::kernel_boot:
        return info().boot_epoch;
    case Metric::grant_lifecycle: {
        if (!grant_) {
            evidence.status("saved grant required", AYNI_NOT_FOUND);
            return 0;
        }
        auto grant = initialized<ayni_grant_info_v1>();
        evidence.status("read original grant", ayni_get_grant(node.kernel(), *grant_, &grant));
        return grant.lifecycle;
    }
    case Metric::writer_advanced:
    case Metric::boot_advanced:
    case Metric::durable_kept: {
        if (!saved_call_) {
            evidence.status("saved epoch required", AYNI_NOT_FOUND);
            return 0;
        }
        if (metric == Metric::writer_advanced) {
            return node.simulation()->writer_epoch() > saved_call_->writer_epoch ? 1 : 0;
        }
        const auto current = info();
        return metric == Metric::boot_advanced ? current.boot_epoch == saved_call_->kernel_boot + 1
                                               : current.durable_seq >= saved_call_->durable_seq;
    }
    case Metric::submissions:
        return service.submissions();
    case Metric::call_durable: {
        const auto current = call();
        return current.requires_seq && current.requires_seq <= info().durable_seq ? 1 : 0;
    }
    case Metric::call_operation:
        return call().operation_id;
    case Metric::task_count:
        return count(AYNI_OBJECT_TASK);
    case Metric::call_query_conclusion:
        return call().query_conclusion;
    case Metric::memory_count: {
        const ayni_memory_search_v1 query{
            sizeof(query), AYNI_MEMORY_PRIVATE, instance(1), 0, {}, {}};
        std::array<ayni_memory_info_v1, goal_capacity> records{};
        uint32_t size = 0;
        evidence.status("search derived memories",
                        ayni_search_memory(node.kernel(), session_, &query, records.data(),
                                           records.size(), &size));
        for (uint32_t at = 0; at < size; ++at) {
            evidence.equal("one version of derived summary", records[at].revision, 1);
            evidence.equal("derived source", records[at].source, AYNI_MEMORY_DERIVED);
        }
        return size;
    }
    case Metric::checkpoint_value: {
        uint8_t value = 0;
        uint32_t bytes = 0;
        evidence.status("copy task checkpoint",
                        ayni_copy_task_checkpoint(node.kernel(), call().task_id, &value,
                                                  sizeof(value), &bytes));
        evidence.equal("checkpoint byte count", bytes, 1);
        return value;
    }
    case Metric::task_occurrence:
    case Metric::model_calls_used:
    case Metric::model_call_limit:
    case Metric::task_steps:
    case Metric::task_state: {
        const auto current = call();
        auto task = initialized<ayni_task_info_v1>();
        evidence.status("get task state", ayni_get_task(node.kernel(), current.task_id, &task));
        if (metric == Metric::task_occurrence) {
            return task.occurrence;
        }
        if (metric == Metric::model_calls_used) {
            return task.model_calls_used;
        }
        if (metric == Metric::model_call_limit) {
            return task.model_call_limit;
        }
        return metric == Metric::task_steps ? task.step_count : task.lifecycle;
    }
    case Metric::goal_state: {
        auto goal = initialized<ayni_goal_info_v1>();
        if (!accepted_goal_) {
            evidence.status("accepted goal required", AYNI_NOT_FOUND);
            return 0;
        }
        evidence.status("get goal state",
                        ayni_get_goal(node.kernel(), accepted_goal_->result.object.id, &goal));
        return goal.lifecycle;
    }
    }
    evidence.status("unknown metric", AYNI_INVALID_ARGUMENT);
    return 0;
}
} // namespace ayni::runtime::experiments
