#include "experiment_execution.h"
#include "experiment_support.h"
#include <algorithm>
#include <cstring>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t long_running_samples = 100000;
constexpr uint32_t checkpoint_bytes = 1;
constexpr uint32_t command_age_us = 100000;
constexpr uint64_t control_field = 1;
constexpr int64_t turn_value = 5;
constexpr int64_t turn_limit = 100;
ayni_catalog_name_v1 name(const char *value) {
    ayni_catalog_name_v1 result{};
    result.size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(result.data, value, result.size);
    return result;
}
ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 required{
        sizeof(required), 1, text("mock.turn"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &required);
}
} // namespace
Physical::Physical(World &world) : world_(world) {}
bool Physical::configure() {
    providers::mock_execution::Config config;
    config.clock_context = &world_;
    config.now_us = [](void *ctx) {
        return static_cast<World *>(ctx)->node.simulation()->now_us();
    };
    config.ticks_to_complete = long_running_samples;
    config.limits.count = 1;
    config.limits.fields[0] = {name("yaw"), name("mrad"), -turn_limit, turn_limit};
    return world_.evidence.status("configure physical deployment", executor.configure(config));
}
// This complete finite registration is the composition root; it depends only on
// public contracts and the existing executor. No physical rule enters Core.
bool Physical::register_profile(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.strategy_id = text("turn.policy");
    strategy.version = 1;
    strategy.decide = decide;
    strategy.ctx = this;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = checkpoint_bytes;
    auto goal = initialized<ayni_goal_type_v1>();
    goal.version = 1;
    goal.goal_type_id = text("turn");
    goal.strategy_id = strategy.strategy_id;
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    goal.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    goal.export_requirements = requirements;
    auto field = initialized<ayni_authority_field_desc_v1>();
    field.field_id = control_field;
    field.usage = AYNI_AUTHORITY_PARAMETER;
    field.domain = initialized<ayni_authority_value_set_v1>();
    field.domain.kind = AYNI_AUTHORITY_I64;
    field.domain.unit = text("mrad");
    field.domain.values.integer = {-turn_limit, turn_limit};
    const ayni_capability_desc_v1 authority{sizeof(authority), 1, text("mock.turn"), 1, 0, &field};
    const ayni_resource_desc_v1 resource{sizeof(resource),
                                         AYNI_RESOURCE_EXCLUSIVE,
                                         text("mock.actuator"),
                                         1,
                                         AYNI_RESOURCE_ACTUATOR,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
    auto provider = initialized<ayni_provider_v1>();
    auto ports = initialized<ayni_executor_v1>();
    if (!e.status("describe physical provider", executor.describe_provider(provider)) ||
        !e.status("describe executor", executor.describe_executor(ports))) {
        return false;
    }
    const ayni_binding_v1 binding{sizeof(binding),
                                  1,
                                  text("turn.binding"),
                                  authority.capability_id,
                                  1,
                                  1,
                                  provider.provider_id,
                                  provider.endpoint_id,
                                  {}};
    const ayni_observation_source_v1 source{sizeof(source),
                                            1,
                                            text("ready"),
                                            AYNI_OBSERVATION_BOOL,
                                            AYNI_OBSERVATION_MEASURED,
                                            text("bool"),
                                            observation_age_us};
    const ayni_scene_rule_v1 rule{
        authority.capability_id, 1, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
    const ayni_observation_condition_v1 condition{text("ready"), 1, 0, 1, 1, observation_age_us};
    const ayni_scene_desc_v1 scene{sizeof(scene), 1, text("look"), 1, 1, &rule, &condition};
    const ayni_execution_field_v1 mapping{text("yaw"), text("mrad"), control_field, 1};
    const ayni_execution_binding_v1 execution{sizeof(execution),
                                              1,
                                              binding.binding_id,
                                              ports.executor_id,
                                              1,
                                              1,
                                              text("look"),
                                              AYNI_FORM_SESSION,
                                              lease_us,
                                              command_age_us,
                                              1,
                                              &mapping};
    return e.status("register physical strategy", ayni_register_strategy(kernel, &strategy)) &&
           e.status("register physical goal", ayni_register_goal_type(kernel, &goal)) &&
           e.status("register physical authority", ayni_register_capability(kernel, &authority)) &&
           e.status("register actuator", ayni_register_resource(kernel, &resource)) &&
           e.status("actuator online",
                    ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE)) &&
           e.status("register physical call",
                    ayni_register_call_capability(kernel, provider.capabilities)) &&
           e.status("register physical provider", ayni_register_provider(kernel, &provider)) &&
           e.status("register physical binding", ayni_register_binding(kernel, &binding)) &&
           e.status("register trusted ready source",
                    ayni_register_observation_source(kernel, &source)) &&
           e.status("register physical scene", ayni_register_scene(kernel, &scene)) &&
           e.status("register executor", ayni_register_executor(kernel, &ports)) &&
           e.status("register execution mapping",
                    ayni_register_execution_binding(kernel, &execution));
}
ayni_status_t Physical::decide(void *, const ayni_task_snapshot_v1 *snapshot,
                               ayni_policy_output_v1 *output) {
    auto result = initialized<ayni_task_call_result_v1>();
    const auto found = snapshot->call_result(snapshot->call_context, text("turn"), &result);
    if (found == AYNI_OK) {
        auto &intent = output->intents[0];
        intent = initialized<ayni_policy_intent_v1>();
        intent.kind = result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                            : AYNI_POLICY_REPORT_FAILURE;
        intent.expected_revision = snapshot->revision;
        output->intent_count = 1;
        return AYNI_OK;
    }
    if (found != AYNI_NOT_FOUND) {
        return found;
    }
    auto field = initialized<ayni_scope_field_v1>();
    field.field_id = control_field;
    field.allowed = initialized<ayni_authority_value_set_v1>();
    field.allowed.kind = AYNI_AUTHORITY_I64;
    field.allowed.unit = text("mrad");
    field.allowed.values.integer = {turn_value, turn_value};
    const ayni_call_request_v1 request{
        sizeof(request),      AYNI_POLICY_REQUEST_CALL,           1, 0, text("turn"),
        text("turn.binding"), {sizeof(ayni_scope_v1), 1, &field}, {}};
    return output->request_call(output->call_context, &request);
}
bool Physical::prepare() {
    if (!world_.grant(1, "mock.turn")) {
        return false;
    }
    auto scene = initialized<ayni_scene_info_v1>();
    if (!world_.evidence.status("read scene",
                                ayni_get_scene(world_.node.kernel(), text("look"), &scene))) {
        return false;
    }
    const ayni_scene_set_enabled_v1 enable{sizeof(enable), 1, text("look"), 1, 0};
    world_.submit(AYNI_INTENT_SCENE_SET_ENABLED, &enable, 0, scene.revision);
    return world_.settle() && publish(1);
}
bool Physical::publish(int64_t value) {
    const ayni_observation_v1 sample{sizeof(sample),
                                     1,
                                     text("ready"),
                                     1,
                                     ++observation_sequence_,
                                     world_.node.simulation()->now_us(),
                                     observation_age_us,
                                     AYNI_OBSERVATION_GOOD,
                                     0,
                                     value};
    return world_.evidence.status("publish local readiness",
                                  ayni_publish_observation(world_.node.kernel(), &sample));
}
ayni_control_info_v1 Physical::info() const {
    auto state = initialized<ayni_control_info_v1>();
    world_.evidence.status("read independent gate", executor.info(state));
    return state;
}
bool Physical::advance() {
    if (!world_.evidence.status("local control cycle", executor.advance())) {
        return false;
    }
    const auto state = info();
    auto &e = world_.evidence;
    if (!nonce_origin_ && state.phase == AYNI_CONTROL_ACTIVE) {
        nonce_origin_ = state.deadline_us - lease_us;
        e.fact("execution", "first_nonce_issued_at_us", *nonce_origin_);
    }
    e.changed("execution:phase", state.phase);
    e.changed("execution:deadline_us", state.deadline_us);
    e.changed("execution:samples", executor.samples());
    e.changed("execution:renewals", executor.renewals());
    e.changed("execution:submissions", executor.submissions());
    e.changed("execution:stop_confirmed_at_us", state.stop_confirmed_at_us);
    if (state.phase == AYNI_CONTROL_STOPPED) {
        if (stopped_samples_ &&
            !e.equal("stopped gate emits no output", executor.samples(), *stopped_samples_)) {
            return false;
        }
        stopped_samples_ = executor.samples();
    }
    return true; // Cleanup can still measure/stop after a previously latched case failure.
}
bool Physical::elapse(uint32_t duration_us) {
    while (duration_us != 0 && world_.evidence.ok()) {
        const auto delta = std::min<uint32_t>(duration_us, tick_us);
        world_.node.advance_clock(delta);
        duration_us -= delta;
        if (!advance()) {
            return false;
        }
    }
    return world_.evidence.ok();
}
bool Physical::at(uint32_t offset_us) {
    if (!nonce_origin_) {
        return world_.evidence.status("active permit origin required", AYNI_NOT_READY);
    }
    const auto target = *nonce_origin_ + offset_us;
    const auto now = world_.node.simulation()->now_us();
    if (target < now) {
        return world_.evidence.status("monotonic experiment time", AYNI_STALE);
    }
    return elapse(static_cast<uint32_t>(target - now));
}
bool Physical::mark_invalidated() {
    invalidated_at_ = world_.node.simulation()->now_us();
    world_.evidence.fact("execution", "invalidated_at_us", *invalidated_at_);
    return world_.evidence.ok();
}
bool Physical::stopped_within_bound() {
    const auto state = info();
    auto &e = world_.evidence;
    if (!invalidated_at_) {
        return e.status("invalidation time required", AYNI_NOT_READY);
    }
    e.fact("execution", "required_stop_by_us", *invalidated_at_ + lease_us);
    return e.equal("independent measured stop", state.phase, AYNI_CONTROL_STOPPED) &&
           e.equal("stop no later than invalidation plus lease",
                   state.stop_confirmed_at_us <= *invalidated_at_ + lease_us, 1) &&
           elapse(static_cast<uint32_t>(tick_us)) &&
           e.equal("no output after measured stop", executor.samples(), *stopped_samples_);
}
bool Physical::change_live(Invalidation kind) {
    ayni_control_observation_v1 live{};
    if (!world_.evidence.status("read live generations", executor.observation(live))) {
        return false;
    }
    if (kind == Invalidation::kernel) {
        const auto boot = world_.info().boot_epoch;
        if (!world_.evidence.equal("restarted Kernel has a new generation",
                                   boot > live.epochs.kernel_boot_epoch, 1)) {
            return false;
        }
        live.epochs.kernel_boot_epoch = boot;
    } else if (kind == Invalidation::calibration) {
        ++live.epochs.calibration_version;
    } else {
        live.interlocks_ok = 0;
        ++live.epochs.safety_epoch;
    }
    return mark_invalidated() &&
           world_.evidence.status("change measured generations", executor.observe(live));
}
bool Physical::invalidate_kernel() {
    return change_live(Invalidation::kernel);
}
bool Physical::invalidate_calibration() {
    return change_live(Invalidation::calibration);
}
bool Physical::estop() {
    return change_live(Invalidation::estop);
}
bool Physical::estop_before_dispatch() {
    return world_.evidence.status("measured local emergency interlock",
                                  executor.set_interlock(0)) &&
           publish(0);
}
bool Physical::reset_estop() {
    ayni_control_observation_v1 live{};
    if (!world_.evidence.status("read reset generations", executor.observation(live))) {
        return false;
    }
    live.interlocks_ok = 1;
    return world_.evidence.status("local reset preserves advanced safety epoch",
                                  executor.observe(live));
}
bool Physical::replay_permit_rejected() {
    const auto before = executor.samples();
    const auto status = executor.replay_permit();
    world_.evidence.fact("execution", "replayed_permit_status", static_cast<uint64_t>(-status));
    return world_.evidence.equal("actual old permit rejected", status != AYNI_OK, 1) &&
           world_.evidence.equal("replay produces no sample", executor.samples(), before);
}
bool Physical::replay_renewal_rejected() {
    const auto status = executor.replay_renewal();
    return world_.evidence.equal("actual old renewal exists", status != AYNI_NOT_FOUND, 1) &&
           world_.evidence.equal("actual old renewal rejected", status != AYNI_OK, 1);
}
bool Physical::unpermitted_rejected() {
    const auto before = executor.samples();
    return world_.evidence.equal("unpermitted sample rejected",
                                 executor.command_without_permit() != AYNI_OK, 1) &&
           world_.evidence.equal("unpermitted sample causes no output", executor.samples(), before);
}
bool Physical::deliver_renewal(ayni_status_t expected) {
    return world_.evidence.equal("actual held renewal exists",
                                 executor.held(AYNI_EXECUTOR_RENEW) != 0, 1) &&
           world_.evidence.status("deliver held renewal packet", executor.answer_renewals()) &&
           world_.evidence.status("gate renewal verdict", executor.last_renewal_status(), expected);
}
bool Physical::drop_renewal() {
    if (!world_.evidence.equal("lost renewal actually existed",
                               executor.held(AYNI_EXECUTOR_RENEW) != 0, 1)) {
        return false;
    }
    world_.evidence.fact("fault", "lost_renewal_packets", executor.held(AYNI_EXECUTOR_RENEW));
    executor.drop_requests();
    return true;
}
bool Physical::power_cycle() {
    return mark_invalidated() &&
           world_.evidence.status("executor power cycle", executor.power_cycle()) &&
           world_.evidence.equal("executor boots disabled", info().phase, AYNI_CONTROL_IDLE);
}
uint64_t Physical::deadline_offset() const {
    return nonce_origin_ ? info().deadline_us - *nonce_origin_ : 0;
}
} // namespace ayni::runtime::experiments
