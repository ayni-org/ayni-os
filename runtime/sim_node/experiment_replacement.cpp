#include "experiment_replacement.h"
#include <algorithm>
#include <cstring>
namespace ayni::runtime::experiments {
namespace {
constexpr uint64_t model_delay_us = 1000;
constexpr uint32_t text_bytes = 512;
constexpr uint32_t common_steps = 2; // Accepted increment + checkpoint.ready.
constexpr uint32_t unknown_strategy_version = 2;
const ayni_checkpoint_reader_v1 rule_reader{text("replacement.rules"), 1, 1};
constexpr char private_body[] = "SYNTHETIC_PRIVATE_E3";
} // namespace
bool Replacement::register_profile(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    if (!offers_) {
        const auto clock = world_.node.clock();
        if (!e.status("configure replacement A",
                      a_.configure(providers::mock::Variant::a, model_delay_us, clock, std::nullopt,
                                   providers::mock::Service::text, bodies_.store())) ||
            !e.status("configure replacement B",
                      b_.configure(providers::mock::Variant::b, model_delay_us, clock, std::nullopt,
                                   providers::mock::Service::text, bodies_.store()))) {
            return false;
        }
        offers_ = Offers{a_.describe(), b_.describe()};
    }
    if (!configured_) {
        auto memory = loop_workspace_.create(strategies::ToolLoop::workspace_requirements);
        if (memory == AYNI_OK) {
            memory =
                conversation_workspace_.create(strategies::Conversation::workspace_requirements);
        }
        if (!e.status("allocate bounded policy workspace", memory)) {
            return false;
        }
        strategies::ToolLoopConfig config;
        config.identity = {text("replacement.loop"), 1, &rule_reader, 1};
        config.model = {AYNI_POLICY_REQUEST_MODEL,
                        text("model"),
                        1,
                        {sizeof(ayni_scope_v1), 0, nullptr},
                        text("Reply briefly.")};
        config.summary = config.model;
        config.limits = {32768, 1024, 8192, 4096};
        config.bodies = bodies_.store();
        if (!e.status("configure replacement ToolLoop",
                      loop_.configure(config, loop_workspace_.get()))) {
            return false;
        }
        configured_ = true;
    }
    auto strategy = loop_.describe();
    strategy.ctx = this;
    strategy.decide = loop;
    if (selected_ == Policy::rules || selected_ == Policy::incompatible) {
        strategy = applications::experiment_fixtures::replacement::rule_strategy();
    } else if (selected_ == Policy::unknown_version) {
        strategy.version = unknown_strategy_version;
        strategy.reader_count = 0;
        strategy.readers = nullptr;
    }
    const auto &offer = provider_ == providers::mock::Variant::a ? offers_->a : offers_->b;
    const ayni_resource_desc_v1 slot{
        sizeof(slot), AYNI_RESOURCE_SHARED, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
    const ayni_capability_desc_v1 capability{
        sizeof(capability), 1, text("mock.text"), 0, 0, nullptr};
    const ayni_binding_v1 binding{sizeof(binding),
                                  1,
                                  text("model"),
                                  text("mock.text"),
                                  1,
                                  1,
                                  offer.provider_id,
                                  offer.endpoint_id,
                                  {},
                                  0,
                                  0,
                                  nullptr};
    e.fact("replacement", "provider_variant", static_cast<uint64_t>(provider_));
    e.fact("replacement", "policy", static_cast<uint64_t>(selected_));
    return e.status("register replacement slot", ayni_register_resource(kernel, &slot)) &&
           e.status("replacement slot online",
                    ayni_set_resource_health(kernel, slot.resource_id, AYNI_RESOURCE_ONLINE)) &&
           e.status("register replacement authority",
                    ayni_register_capability(kernel, &capability)) &&
           e.status("register replacement call",
                    ayni_register_call_capability(kernel, offer.capabilities)) &&
           e.status("register replacement A", ayni_register_provider(kernel, &offers_->a)) &&
           e.status("register replacement B", ayni_register_provider(kernel, &offers_->b)) &&
           e.status("register replacement binding", ayni_register_binding(kernel, &binding)) &&
           e.status("register replacement strategy", ayni_register_strategy(kernel, &strategy)) &&
           e.status("register replacement goals",
                    applications::experiment_fixtures::replacement::register_goals(
                        kernel, strategy.strategy_id, strategies::Conversation::validate_turn,
                        const_cast<ayni_workspace_v1 *>(conversation_workspace_.get())));
}
ayni_status_t Replacement::loop(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                                ayni_policy_output_v1 *output) {
    bool handled = false;
    const auto status =
        applications::experiment_fixtures::replacement::prerequisite(*snapshot, *output, handled);
    if (handled || status != AYNI_OK) {
        return status;
    }
    const auto strategy = static_cast<Replacement *>(ctx)->loop_.describe();
    return strategy.decide(strategy.ctx, snapshot, output);
}
bool Replacement::advance() {
    if (!deliver_) {
        return true;
    }
    auto &e = world_.evidence;
    const bool ok =
        e.status("replacement advance A", a_.advance(world_.node.kernel(), barriers_)) &&
        e.status("replacement advance B", b_.advance(world_.node.kernel(), barriers_));
    e.changed("replacement:a_executions", a_.executions());
    e.changed("replacement:b_executions", b_.executions());
    return ok;
}
void Replacement::abandon_kernel() {
    a_.abandon_kernel();
    b_.abandon_kernel();
}
ayni_status_t Replacement::close() {
    const auto a = a_.close(), b = b_.close();
    if (a == AYNI_OK && b == AYNI_OK) {
        offers_.reset();
    }
    return a != AYNI_OK ? a : b;
}
uint64_t Replacement::create(const char *type) {
    strategies::ConversationRecord record;
    record.id = world_.last_command() + 1;
    record.owner_instance_id = world_.instance(1);
    strategies::Encoded parameters;
    if (!world_.evidence.status(
            "replacement conversation",
            strategies::Conversation::begin(conversation_workspace_.get(), *bodies_.store(), record,
                                            text("fixture input"), parameters))) {
        return 0;
    }
    auto goal = initialized<ayni_goal_create_v1>();
    goal.owner_instance_id = record.owner_instance_id;
    goal.goal_type_id = text(type);
    goal.goal_type_version = 1;
    goal.trigger = AYNI_TRIGGER_ONCE;
    goal.parameters = parameters.view();
    return world_.submit(AYNI_INTENT_GOAL_CREATE, &goal).object.id;
}
ayni_task_info_v1 Replacement::task(uint64_t goal) const {
    uint64_t ids[operation_capacity]{};
    uint32_t size = 0;
    auto out = initialized<ayni_task_info_v1>();
    if (!world_.evidence.status("replacement list tasks",
                                ayni_list_objects(world_.node.kernel(), AYNI_OBJECT_TASK, ids,
                                                  operation_capacity, &size))) {
        return out;
    }
    for (uint32_t i = 0; i < size; ++i) {
        if (!world_.evidence.status("replacement task",
                                    ayni_get_task(world_.node.kernel(), ids[i], &out))) {
            break;
        }
        if (out.goal_id == goal) {
            return out;
        }
    }
    return initialized<ayni_task_info_v1>();
}
ayni_call_info_v1 Replacement::call(uint64_t id) const {
    auto out = initialized<ayni_call_info_v1>();
    world_.evidence.status("replacement get call", ayni_get_call(world_.node.kernel(), id, &out));
    return out;
}
ayni_call_info_v1 Replacement::action(uint64_t task_id) const {
    uint64_t ids[operation_capacity]{};
    uint32_t size = 0;
    if (world_.evidence.status("replacement list calls",
                               ayni_list_objects(world_.node.kernel(), AYNI_OBJECT_CALL, ids,
                                                 operation_capacity, &size))) {
        for (uint32_t i = 0; i < size; ++i) {
            auto out = call(ids[i]);
            if (out.task_id == task_id && out.effect == AYNI_CALL_EFFECT_COMPLETE) {
                return out;
            }
        }
    }
    return initialized<ayni_call_info_v1>();
}
ayni_memory_info_v1 Replacement::memory() const {
    auto out = initialized<ayni_memory_info_v1>();
    char body[text_bytes]{};
    uint32_t size = 0;
    auto &e = world_.evidence;
    e.status("replacement read private memory",
             ayni_get_memory(world_.node.kernel(), world_.owner_session(), memory_, &out, body,
                             sizeof(body), &size));
    e.equal("private body preserved",
            size == sizeof(private_body) - 1 && std::memcmp(body, private_body, size) == 0, 1);
    return out;
}
bool Replacement::change_goal(uint64_t id, ayni_intent_kind_t kind) {
    auto goal = initialized<ayni_goal_info_v1>();
    if (!world_.evidence.status("replacement get goal",
                                ayni_get_goal(world_.node.kernel(), id, &goal))) {
        return false;
    }
    const ayni_goal_target_v1 target{sizeof(target), 0, id};
    const auto result = world_.submit(kind, &target, 0, goal.revision);
    return world_.until("replacement goal transition durable",
                        [&] { return result.commit_seq <= world_.info().durable_seq; });
}
// Stop at a real durable common checkpoint while another Task has an accepted
// model delivery. The external counter has completed exactly one operation.
bool Replacement::prepare() {
    auto &e = world_.evidence;
    if (!world_.grant() || !world_.grant(1, "mock.text")) {
        return false;
    }
    const ayni_memory_save_v1 body{sizeof(body),
                                   AYNI_MEMORY_PRIVATE,
                                   0,
                                   world_.instance(1),
                                   AYNI_PROFILE_NONE,
                                   0,
                                   text(private_body),
                                   nullptr,
                                   0,
                                   0,
                                   nullptr};
    const auto saved = world_.submit(AYNI_INTENT_MEMORY_SAVE, &body);
    memory_ = saved.object.id;
    if (!world_.until("replacement memory durable",
                      [&] { return saved.commit_seq <= world_.info().durable_seq; })) {
        return false;
    }
    world_.items_per_step = 1;
    workflow_ = create("replacement.workflow");
    if (!world_.until("replacement action dispatched",
                      [&] { return world_.service.pending() == 1; }) ||
        !e.status("replacement execute counter", world_.service.execute(world_.node.kernel())) ||
        !e.status("replacement counter receipt",
                  world_.service.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) ||
        !world_.until("common checkpoint applied",
                      [&] { return task(workflow_).step_count == common_steps; }) ||
        !change_goal(workflow_, AYNI_INTENT_GOAL_PAUSE)) {
        return false;
    }
    pending_goal_ = create("replacement.model");
    return world_.until("old model in flight", [&] {
        return a_.pending() == 1;
    }) && world_.until("old model count durable", [&] {
        return task(pending_goal_).commit_seq <= world_.info().durable_seq;
    }) && remember();
}
bool Replacement::remember() {
    Baseline b{
        task(workflow_),    task(pending_goal_),      {}, {}, memory(), {}, world_.instance(1),
        world_.instance(2), world_.info().boot_epoch, {}, {}};
    b.action = action(b.task.task_id);
    b.model = call(b.pending.logical_call_id);
    if (!world_.evidence.status(
            "inspect baseline progress",
            inspect_task_progress(world_.node.kernel(), b.task.task_id, b.progress)) ||
        !world_.evidence.status(
            "inspect baseline route",
            inspect_call_route(world_.node.kernel(), b.model.logical_call_id, b.model_route))) {
        return false;
    }
    world_.evidence.fact("baseline", "checkpoint_writer_version", b.progress.writer_version);
    world_.evidence.fact("baseline", "capability_version", b.model_route.capability_version);
    world_.evidence.fact("baseline", "binding_version", b.model_route.binding_version);
    world_.evidence.fact("baseline", "provider_version", b.model_route.provider_version);
    uint64_t ids[operation_capacity]{};
    uint32_t size = 0;
    if (!world_.evidence.status("baseline grants",
                                ayni_list_objects(world_.node.kernel(), AYNI_OBJECT_GRANT, ids,
                                                  operation_capacity, &size))) {
        return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
        auto grant = initialized<ayni_grant_info_v1>();
        if (!world_.evidence.status("baseline grant",
                                    ayni_get_grant(world_.node.kernel(), ids[i], &grant))) {
            return false;
        }
        b.grants.push_back(grant);
    }
    baseline_ = std::move(b);
    auto &e = world_.evidence;
    e.fact("baseline", "task", baseline_->task.task_id);
    e.fact("baseline", "goal", workflow_);
    e.fact("baseline", "action_operation", baseline_->action.operation_id);
    e.fact("baseline", "model_call", baseline_->model.logical_call_id);
    e.fact("baseline", "model_operation", baseline_->model.operation_id);
    e.fact("baseline", "model_boot", baseline_->model.attempt.boot_epoch);
    e.fact("baseline", "model_request", baseline_->model.attempt.request_id);
    return e.equal("baseline user-paused task", baseline_->task.lifecycle, AYNI_TASK_PAUSED) &&
           e.equal("baseline common steps", baseline_->task.step_count, common_steps) &&
           e.equal("baseline model count", baseline_->pending.model_calls_used, 1) &&
           e.equal("baseline action durable",
                   baseline_->action.requires_seq <= world_.info().durable_seq, 1) &&
           e.equal("baseline counted action", world_.service.executions(), 1);
}
} // namespace ayni::runtime::experiments
