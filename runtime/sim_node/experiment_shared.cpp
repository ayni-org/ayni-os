#include "experiment_shared.h"
#include "experiment_support.h"
#include <algorithm>
#include <ayni/runtime/common/memory_capability.h>
namespace ayni::runtime::experiments {
namespace {
using applications::normal_fixtures::GoalKind;
constexpr char button[] = "shared.button";
} // namespace
bool SharedServices::register_profile(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    e.fact("configuration", "model_delay_us", model_delay_us);
    e.fact("configuration", "model_timeout_ms", model_timeout_ms);
    e.fact("configuration", "resource_wait_us", AYNI_RESOURCE_WAIT_DEFAULT_US);
    e.fact("configuration", "local_response_step_bound", response_step_bound);
    if (!offers_) {
        const auto clock = world_.node.clock();
        if (!e.status("configure model A",
                      model_a_.configure(providers::mock::Variant::a, model_delay_us, clock,
                                         model_timeout_ms)) ||
            !e.status("configure model B",
                      model_b_.configure(providers::mock::Variant::b, model_delay_us, clock,
                                         model_timeout_ms))) {
            return false;
        }
        auto memory = loop_workspace_.create(strategies::ToolLoop::workspace_requirements);
        if (memory == AYNI_OK) {
            memory =
                conversation_workspace_.create(strategies::Conversation::workspace_requirements);
        }
        if (!e.status("allocate bounded policy workspace", memory)) {
            return false;
        }
        strategies::ToolLoopConfig loop;
        loop.identity = {text("shared.tool_loop"), 1, nullptr, 0};
        loop.model = {AYNI_POLICY_REQUEST_MODEL,
                      text("model"),
                      1,
                      {sizeof(ayni_scope_v1), 0, nullptr},
                      text("Reply briefly.")};
        loop.summary = loop.model;
        if (!e.status("configure actual ToolLoop",
                      tool_loop_.configure(loop, loop_workspace_.get()))) {
            return false;
        }
        offers_ = Offers{model_a_.describe(), model_b_.describe()};
    }
    const ayni_resource_desc_v1 resource{
        sizeof(resource), AYNI_RESOURCE_SHARED, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
    const auto &a = offers_->a;
    const ayni_capability_desc_v1 capability{
        sizeof(capability), 1, a.capabilities->capability_id, 0, 0, nullptr};
    const auto memory_fields = memory_access_fields();
    const ayni_capability_desc_v1 memory{sizeof(memory),
                                         1,
                                         text("memory.access"),
                                         static_cast<uint32_t>(memory_fields.size()),
                                         0,
                                         memory_fields.data()};
    const ayni_binding_v1 binding{sizeof(binding),
                                  1,
                                  text("model"),
                                  capability.capability_id,
                                  1,
                                  1,
                                  a.provider_id,
                                  a.endpoint_id,
                                  a.destination_id,
                                  0,
                                  0,
                                  nullptr};
    const auto loop_strategy = tool_loop_.describe();
    auto chat = initialized<ayni_goal_type_v1>();
    chat.goal_type_id = text("shared.chat");
    chat.version = 1;
    chat.strategy_id = loop_strategy.strategy_id;
    chat.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    chat.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    chat.ctx = const_cast<ayni_workspace_v1 *>(conversation_workspace_.get());
    chat.validate = strategies::Conversation::validate_turn;
    chat.export_requirements = [](void *, ayni_bytes_v1,
                                  const ayni_goal_requirement_sink_v1 *sink) {
        const ayni_goal_requirement_v1 model{
            sizeof(model), 1, text("mock.text"), {sizeof(ayni_scope_v1), 0, nullptr}};
        return sink->emit(sink->ctx, &model);
    };
    return e.status("register memory authority", ayni_register_capability(kernel, &memory)) &&
           e.status("register model slot", ayni_register_resource(kernel, &resource)) &&
           e.status("model slot online",
                    ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE)) &&
           e.status("register model authority", ayni_register_capability(kernel, &capability)) &&
           e.status("register model call", ayni_register_call_capability(kernel, a.capabilities)) &&
           e.status("register model A", ayni_register_provider(kernel, &a)) &&
           e.status("register model B", ayni_register_provider(kernel, &offers_->b)) &&
           e.status("register model binding", ayni_register_binding(kernel, &binding)) &&
           e.status(
               "register shared policies",
               policy_.register_all(kernel, applications::normal_fixtures::PolicyRevision::v1)) &&
           e.status("register actual ToolLoop", ayni_register_strategy(kernel, &loop_strategy)) &&
           e.status("register ToolLoop goal", ayni_register_goal_type(kernel, &chat)) &&
           register_resources(kernel) && register_assistant(kernel);
}
bool SharedServices::advance() {
    auto &e = world_.evidence;
    if (!e.status("advance model A", model_a_.advance(world_.node.kernel(), barrier_checks_)) ||
        !e.status("advance model B", model_b_.advance(world_.node.kernel(), barrier_checks_))) {
        return false;
    }
    if (!e.status("advance shared camera", camera_.advance())) {
        return false;
    }
    if (assistant_ && !update_presentation_window()) {
        return false;
    }
    const auto active = model_a_.pending() + model_b_.pending();
    e.changed("shared:model_pending", active);
    e.changed("shared:model_executions", model_a_.executions() + model_b_.executions());
    return e.equal("one model slot bounds accepted borrows", active <= 1, 1);
}
// The Host obeys the public step report: ready/completion work runs now; timers
// wait until the earlier Core or Provider deadline. Idle 1-ms polling is not a
// required step. Every actual Core step still enters the response-step count.
uint64_t SharedServices::clock_delta(const ayni_step_report_v1 &last, uint64_t seen_wakes) const {
    if (last.more_ready || world_.node.simulation()->pending_storage() ||
        world_.node.simulation()->wake_count() != seen_wakes) {
        return 0;
    }
    const auto now = world_.node.simulation()->now_us();
    const auto due = std::min({last.next_deadline_us, model_a_.next_due_us(),
                               model_b_.next_due_us(), camera_.next_due_us()});
    return due == UINT64_MAX ? tick_us : due > now ? due - now : 0;
}
void SharedServices::abandon_kernel() {
    notification_.quiesce();
    microphone_.quiesce();
    group_.quiesce();
    last_opening_.reset();
    camera_.abandon_kernel();
    model_a_.abandon_kernel();
    model_b_.abandon_kernel();
}
ayni_status_t SharedServices::close() {
    const auto camera = camera_.close();
    if (camera == AYNI_OK) {
        last_opening_.reset();
        camera_offer_.reset();
    }
    const auto a = model_a_.close();
    const auto b = model_b_.close();
    if (a == AYNI_OK && b == AYNI_OK) {
        offers_.reset();
    }
    return camera != AYNI_OK ? camera : a != AYNI_OK ? a : b;
}
uint64_t SharedServices::completed() const {
    return state_ == RunState::completed ? 1 : 0;
}
uint64_t SharedServices::create_chat(uint64_t agent) {
    strategies::ConversationRecord conversation;
    conversation.id = 1;
    conversation.owner_instance_id = world_.instance(agent);
    strategies::Encoded parameters;
    if (!world_.evidence.status(
            "encode ToolLoop turn",
            strategies::Conversation::begin(conversation_workspace_.get(), conversation,
                                            text("synthetic question"), parameters))) {
        return 0;
    }
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text("shared.chat");
    body.goal_type_version = 1;
    body.owner_instance_id = conversation.owner_instance_id;
    body.trigger = AYNI_TRIGGER_ONCE;
    body.parameters = parameters.view();
    return world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
}
uint64_t SharedServices::create(GoalKind kind, uint64_t agent, ayni_bytes_v1 parameters) {
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text(applications::normal_fixtures::goal_type(kind));
    body.goal_type_version = 1;
    body.owner_instance_id = world_.instance(agent);
    body.parameters = parameters;
    body.trigger = kind == GoalKind::local ? AYNI_TRIGGER_LOCAL_INPUT : AYNI_TRIGGER_ONCE;
    if (kind == GoalKind::local) {
        body.trigger_source = text(button);
    }
    return world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
}
ayni_task_info_v1 SharedServices::task(uint64_t goal_id) const {
    auto goal = initialized<ayni_goal_info_v1>();
    auto out = initialized<ayni_task_info_v1>();
    auto status = ayni_get_goal(world_.node.kernel(), goal_id, &goal);
    if (status == AYNI_OK && goal.last_task_id) {
        status = ayni_get_task(world_.node.kernel(), goal.last_task_id, &out);
    }
    if (status != AYNI_OK) {
        world_.evidence.status("get shared Task", status);
    }
    return out;
}
ayni_call_info_v1 SharedServices::call(uint64_t goal_id) const {
    auto out = initialized<ayni_call_info_v1>();
    const auto current = task(goal_id);
    std::array<uint64_t, operation_capacity> ids{};
    uint32_t size = 0;
    auto status =
        ayni_list_objects(world_.node.kernel(), AYNI_OBJECT_CALL, ids.data(), ids.size(), &size);
    if (status != AYNI_OK) {
        world_.evidence.status("list shared Calls", status);
        return out;
    }
    for (uint32_t at = 0; at < size; ++at) {
        auto candidate = initialized<ayni_call_info_v1>();
        status = ayni_get_call(world_.node.kernel(), ids[at], &candidate);
        if (status != AYNI_OK) {
            world_.evidence.status("get shared Call", status);
            break;
        }
        if (candidate.task_id == current.task_id) {
            out = candidate;
        }
    }
    return out;
}
ayni_resource_info_v1 SharedServices::model_slot() const {
    return resource("model.slot");
}
bool SharedServices::contention(uint64_t &a, uint64_t &b) {
    if (!world_.grant(1, "mock.text") || !world_.grant(2, "mock.text")) {
        return false;
    }
    b = create_chat(2);
    if (!world_.until("B holds model slot", [&] { return model_a_.pending() == 1; })) {
        return false;
    }
    a = create(GoalKind::model, 1);
    if (!world_.until("A queued behind B", [&] {
            const auto waiting = call(a);
            return waiting.wait_reason == AYNI_CALL_WAIT_OCCUPIED ||
                   waiting.wait_reason == AYNI_CALL_WAIT_FIFO;
        })) {
        return false;
    }
    world_.evidence.fact("shared", "blocked_on_model_slot_call", call(a).logical_call_id);
    return world_.evidence.equal("model slot capacity", model_slot().capacity, 1) &&
           world_.evidence.equal("one model holder during contention", model_slot().used, 1) &&
           world_.evidence.equal("B is still running", task(b).lifecycle, AYNI_TASK_RUNNING) &&
           world_.evidence.equal("A has not acquired a model borrow",
                                 model_a_.pending() + model_b_.pending(), 1);
}
bool SharedServices::finish_models(uint64_t a, uint64_t b) {
    return world_.until("both model tasks complete",
                        [&] {
                            return task(a).lifecycle == AYNI_TASK_COMPLETED &&
                                   task(b).lifecycle == AYNI_TASK_COMPLETED;
                        }) &&
           world_.evidence.equal("two independent model executions",
                                 model_a_.executions() + model_b_.executions(), 2);
}
bool SharedServices::model_competition() {
    uint64_t a = 0, b = 0;
    return contention(a, b) && finish_models(a, b);
}
bool SharedServices::local_rule() {
    const auto local = create(GoalKind::local, 1);
    if (!world_.until("local rule durably installed", [&] {
            auto goal = initialized<ayni_goal_info_v1>();
            return world_.evidence.status("read local rule",
                                          ayni_get_goal(world_.node.kernel(), local, &goal)) &&
                   goal.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    uint64_t a = 0, b = 0;
    if (!contention(a, b)) {
        return false;
    }
    auto input = initialized<ayni_event_v1>();
    input.kind = AYNI_EVENT_LOCAL_INPUT;
    input.source = text(button);
    if (!world_.evidence.status("local button accepted",
                                ayni_post_event(world_.node.kernel(), &input))) {
        return false;
    }
    const auto first_step = world_.steps;
    if (!world_.until(
            "local rule response hard bound",
            [&] {
                const auto done = task(local);
                return done.lifecycle == AYNI_TASK_COMPLETED &&
                       done.commit_seq <= world_.info().durable_seq;
            },
            response_step_bound)) {
        return false;
    }
    const auto used = world_.steps - first_step;
    world_.evidence.fact("shared", "local_response_steps", used);
    return world_.evidence.equal("local response within ten steps", used <= response_step_bound,
                                 1) &&
           world_.evidence.equal("model remains in flight at local completion",
                                 model_a_.pending() + model_b_.pending(), 1) &&
           finish_models(a, b);
}
bool SharedServices::run(SharedScenario scenario) {
    if (!world_.evidence.equal("one scenario per world", static_cast<uint32_t>(state_),
                               static_cast<uint32_t>(RunState::idle))) {
        return false;
    }
    state_ = RunState::running;
    bool result = false;
    switch (scenario) {
    case SharedScenario::delete_blocks_stale_summary:
        result = invalidate_summary(SourceChange::erase);
        break;
    case SharedScenario::user_edit_not_overwritten:
        result = invalidate_summary(SourceChange::edit);
        break;
    case SharedScenario::cleanup_resumes_after_restart:
        result = invalidate_summary(SourceChange::restart_cleanup);
        break;
    case SharedScenario::revoke_blocks_stale_summary:
        result = invalidate_summary(SourceChange::revoke);
        break;
    case SharedScenario::exclusive_queue_shared_parallel:
        result = resources_parallel();
        break;
    case SharedScenario::resource_group_all_or_nothing:
        result = resource_group();
        break;
    case SharedScenario::timeout_cancel_releases:
        result = timeout_cancel();
        break;
    case SharedScenario::close_stops_generation:
        result = subscription_close();
        break;
    case SharedScenario::renewal_rechecks_grant:
        result = subscription_revoke();
        break;
    case SharedScenario::pause_keeps_reminder:
        result = present(PresentationCase::pause);
        break;
    case SharedScenario::presentation_success_completes:
        result = present(PresentationCase::success);
        break;
    case SharedScenario::presentation_failed_pending:
        result = present(PresentationCase::failed);
        break;
    case SharedScenario::presentation_unknown_pending:
        result = present(PresentationCase::unknown);
        break;
    case SharedScenario::duplicate_receipt_once:
        result = present(PresentationCase::duplicate);
        break;
    case SharedScenario::receipt_lost_reconciled:
        result = present(PresentationCase::lost);
        break;
    case SharedScenario::quiet_hours_deferred:
        result = present(PresentationCase::quiet);
        break;
    case SharedScenario::estop_keeps_reminder:
        result = present(PresentationCase::estop);
        break;
    case SharedScenario::completion_slots_when_queue_full:
        result = completion_pressure();
        break;
    case SharedScenario::ordinary_work_expires_with_reason:
        result = ordinary_expiry();
        break;
    case SharedScenario::model_slot_contention:
        result = model_competition();
        break;
    case SharedScenario::local_rule_progresses:
        result = local_rule();
        break;
    case SharedScenario::private_read_denied:
        result = privacy_denial();
        break;
    case SharedScenario::explicit_share_then_revoke:
        result = share_then_revoke();
        break;
    case SharedScenario::profile_scoped_to_agent:
        result = profile_scope();
        break;
    case SharedScenario::voice_cannot_grant:
        result = voice_boundary();
        break;
    case SharedScenario::model_limit_two:
        result = model_limit();
        break;
    case SharedScenario::restart_keeps_model_calls:
        result = restart_limit();
        break;
    }
    if (result && world_.evidence.ok()) {
        state_ = RunState::completed;
    }
    return result;
}
} // namespace ayni::runtime::experiments

namespace ayni::runtime::experiments {
bool SharedServices::begin_close() {
    return world_.evidence.status("drain simulated microphone", microphone_.drain()) &&
           world_.evidence.status("drain simulated group", group_.drain()) &&
           world_.evidence.status("drain simulated presenter", notification_.drain());
}
} // namespace ayni::runtime::experiments
