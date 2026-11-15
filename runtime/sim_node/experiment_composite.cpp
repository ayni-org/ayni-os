#include "experiment_composite.h"
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/runtime/common/memory_capability.h>
#include <cstring>
namespace ayni::runtime::experiments {
namespace {
// Keep the finite media store below capacity while durable subscription ACKs
// advance. A 1-ms stream would fill all eight TTL-retained frames before policy.
constexpr uint64_t frame_interval_us = 1000000;
constexpr char source_body[] = "SYNTHETIC_E4_SOURCE";
} // namespace
bool Composite::register_profile(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    e.fact("catalog", "strategy_version", strategy_version_);
    e.fact("catalog", "goal_type_version", goal_version_);
    if (!model_offer_) {
        if (!e.status("configure composite model",
                      model_.configure(providers::mock::Variant::a, model_delay_us,
                                       world_.node.clock()))) {
            return false;
        }
        model_offer_ = model_.describe();
    }
    if (!camera_offer_) {
        providers::mock::SubscriptionConfig config;
        const auto clock = world_.node.clock();
        config.clock_context = clock.context;
        config.now_us = clock.now_us;
        config.frame_interval_us = frame_interval_us;
        if (!e.status("configure composite camera", camera_.configure(config))) {
            return false;
        }
        ayni_provider_v1 offer{};
        if (!e.status("describe composite camera", camera_.describe(offer))) {
            return false;
        }
        camera_offer_ = offer;
    }
    if (!tools_offer_) {
        if (!e.status("configure composite tools model",
                      tools_model_.configure(providers::mock::Variant::a, model_delay_us,
                                             world_.node.clock(), std::nullopt,
                                             providers::mock::Service::tools))) {
            return false;
        }
        tools_offer_ = tools_model_.describe();
    }
    for (const char *id : {"model.slot", "camera.slot", "notification.slot"}) {
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text(id), 1, 0, 0, 0, nullptr, {}};
        if (!e.status("register composite resource", ayni_register_resource(kernel, &resource)) ||
            !e.status(
                "composite resource online",
                ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE))) {
            return false;
        }
    }
    auto fields = memory_access_fields();
    const ayni_capability_desc_v1 memory{
        sizeof(memory), 1, text("memory.access"), static_cast<uint32_t>(fields.size()), 0,
        fields.data()};
    if (!e.status("register composite memory authority",
                  ayni_register_capability(kernel, &memory))) {
        return false;
    }
    auto camera = *camera_offer_;
    camera.ctx = this;
    camera.submit = camera_submit;
    camera.cancel = [](void *ctx, ayni_attempt_ref_v1 attempt) {
        const auto &offer = *static_cast<Composite *>(ctx)->camera_offer_;
        return offer.cancel(offer.ctx, attempt);
    };
    camera.renew = [](void *ctx, const ayni_subscription_renew_v1 *request) {
        const auto &offer = *static_cast<Composite *>(ctx)->camera_offer_;
        return offer.renew(offer.ctx, request);
    };
    const ayni_provider_v1 offers[] = {*model_offer_, camera, notification_.describe(),
                                       *tools_offer_};
    const char *const aliases[] = {"model", "observe", "notify", "tools.model"};
    for (uint32_t i = 0; i < std::size(offers); ++i) {
        const auto &offer = offers[i];
        const ayni_capability_desc_v1 authority{
            sizeof(authority), 1, offer.capabilities->capability_id, 0, 0, nullptr};
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text(aliases[i]),
                                      authority.capability_id,
                                      1,
                                      1,
                                      offer.provider_id,
                                      offer.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        if (!e.status("register composite authority",
                      ayni_register_capability(kernel, &authority)) ||
            !e.status("register composite call",
                      ayni_register_call_capability(kernel, offer.capabilities)) ||
            !e.status("register composite provider", ayni_register_provider(kernel, &offer)) ||
            !e.status("register composite binding", ayni_register_binding(kernel, &binding))) {
            return false;
        }
    }
    auto source = initialized<ayni_observation_source_v1>();
    return e.status("describe composite source", camera_.describe_source(source)) &&
           e.status("register composite source",
                    ayni_register_observation_source(kernel, &source)) &&
           e.status("register composite workflow",
                    applications::experiment_fixtures::composite::register_workflow(
                        kernel, strategy_version_, goal_version_));
}
ayni_submit_result_t Composite::camera_submit(void *ctx, const ayni_provider_request_v1 *request,
                                              ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Composite *>(ctx);
    const auto &offer = *self.camera_offer_;
    const auto status = offer.submit(offer.ctx, request, completion);
    if (status != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        self.opening_ = *request->subscription;
    }
    return status;
}
bool Composite::advance() {
    auto &e = world_.evidence;
    if (!e.status("advance composite tools model",
                  tools_model_.advance(world_.node.kernel(), barriers_))) {
        return false;
    }
    if (model_delivery_ &&
        !e.status("advance composite model", model_.advance(world_.node.kernel(), barriers_))) {
        return false;
    }
    if (camera_delivery_ && !e.status("advance composite camera", camera_.advance())) {
        return false;
    }
    if (closing_ && !e.status("drain composite presenter", notification_.drain())) {
        return false;
    }
    e.changed("composite:model_executions", model_.executions());
    e.changed("composite:presentations", notification_.executions());
    return true;
}
void Composite::abandon_kernel() {
    model_.abandon_kernel();
    tools_model_.abandon_kernel();
    camera_.abandon_kernel();
    notification_.quiesce();
    opening_.reset();
}
void Composite::begin_close() {
    closing_ = model_delivery_ = camera_delivery_ = true;
}
ayni_status_t Composite::close() {
    const auto model = model_.close(), camera = camera_.close(), tools = tools_model_.close();
    if (model == AYNI_OK) {
        model_offer_.reset();
    }
    if (camera == AYNI_OK) {
        camera_offer_.reset();
        opening_.reset();
    }
    if (tools == AYNI_OK) {
        tools_offer_.reset();
    }
    return model != AYNI_OK ? model : camera != AYNI_OK ? camera : tools;
}
uint64_t Composite::save_source(uint64_t agent) {
    const ayni_memory_save_v1 body{sizeof(body),
                                   AYNI_MEMORY_PRIVATE,
                                   0,
                                   world_.instance(agent),
                                   AYNI_PROFILE_NONE,
                                   0,
                                   text(source_body),
                                   nullptr,
                                   0,
                                   0,
                                   nullptr};
    const auto saved = world_.submit(AYNI_INTENT_MEMORY_SAVE, &body);
    return world_.until("composite source durable",
                        [&] { return saved.commit_seq <= world_.info().durable_seq; })
               ? saved.object.id
               : 0;
}
uint64_t Composite::create(uint64_t source, uint64_t agent) {
    const auto parameters = std::to_string(source);
    auto goal = initialized<ayni_goal_create_v1>();
    goal.goal_type_id = text("composite_workflow");
    goal.goal_type_version = 1;
    goal.owner_instance_id = world_.instance(agent);
    goal.parent_goal_id = parent_;
    goal.trigger = AYNI_TRIGGER_ONCE;
    goal.parameters = {reinterpret_cast<const uint8_t *>(parameters.data()),
                       static_cast<uint32_t>(parameters.size())};
    return world_.submit(AYNI_INTENT_GOAL_CREATE, &goal).object.id;
}
ayni_task_info_v1 Composite::task(std::optional<uint64_t> selected) const {
    auto goal = initialized<ayni_goal_info_v1>();
    auto result = initialized<ayni_task_info_v1>();
    if (world_.evidence.status("composite Goal", ayni_get_goal(world_.node.kernel(),
                                                               selected.value_or(goal_), &goal)) &&
        goal.last_task_id) {
        world_.evidence.status("composite Task",
                               ayni_get_task(world_.node.kernel(), goal.last_task_id, &result));
    }
    return result;
}
ayni_call_info_v1 Composite::call(const char *step) const {
    auto result = initialized<ayni_call_info_v1>();
    const auto task_id = task().task_id;
    uint64_t ids[operation_capacity]{};
    uint32_t count = 0;
    if (!world_.evidence.status("list composite calls",
                                ayni_list_objects(world_.node.kernel(), AYNI_OBJECT_CALL, ids,
                                                  operation_capacity, &count))) {
        return result;
    }
    for (uint32_t i = 0; i < count; ++i) {
        auto value = initialized<ayni_call_info_v1>();
        if (!world_.evidence.status("get composite call",
                                    ayni_get_call(world_.node.kernel(), ids[i], &value))) {
            return result;
        }
        if (value.task_id != task_id) {
            continue;
        }
        CallRouteEvidence route;
        if (!world_.evidence.status("inspect composite step",
                                    inspect_call_route(world_.node.kernel(), ids[i], route))) {
            return result;
        }
        if (route.step == step) {
            return value;
        }
    }
    return result;
}
ayni_subscription_info_v1 Composite::subscription() const {
    auto out = initialized<ayni_subscription_info_v1>();
    const auto current = call("capture.v1");
    if (current.logical_call_id) {
        world_.evidence.status(
            "composite subscription",
            ayni_get_subscription(world_.node.kernel(), current.logical_call_id, &out));
    }
    return out;
}
uint32_t Composite::candidates() const {
    const ayni_memory_search_v1 query{
        sizeof(query), AYNI_MEMORY_PRIVATE, world_.instance(1), 0, {}, {}};
    std::array<ayni_memory_info_v1, operation_capacity> rows{};
    uint32_t size = 0, count = 0;
    if (!world_.evidence.status("search composite candidate memory",
                                ayni_search_memory(world_.node.kernel(), world_.owner_session(),
                                                   &query, rows.data(),
                                                   static_cast<uint32_t>(rows.size()), &size))) {
        return 0;
    }
    for (uint32_t i = 0; i < size; ++i) {
        if (rows[i].source == AYNI_MEMORY_MODEL || rows[i].source == AYNI_MEMORY_DERIVED) {
            ++count;
        }
    }
    return count;
}
bool Composite::change(uint64_t id, ayni_intent_kind_t kind) {
    auto goal = initialized<ayni_goal_info_v1>();
    if (!world_.evidence.status("controlled composite Goal",
                                ayni_get_goal(world_.node.kernel(), id, &goal))) {
        return false;
    }
    const ayni_goal_target_v1 body{sizeof(body), 0, id};
    const auto changed = world_.submit(kind, &body, 0, goal.revision);
    return world_.until("composite control durable",
                        [&] { return changed.commit_seq <= world_.info().durable_seq; });
}
bool Composite::prepare(Preparation preparation) {
    for (const char *capability :
         {"test.increment", "mock.observe", "mock.text", "notify.present", "mock.turn"}) {
        if (!world_.grant(1, capability)) {
            return false;
        }
    }
    auto parent = initialized<ayni_goal_create_v1>();
    parent.goal_type_id = text("composite.parent");
    parent.goal_type_version = 1;
    parent.owner_instance_id = world_.instance(1);
    parent.trigger = AYNI_TRIGGER_NONE;
    parent_ = world_.submit(AYNI_INTENT_GOAL_CREATE, &parent).object.id;
    source_ = save_source(source_owner_);
    if (source_owner_ != 1) {
        std::array<ayni_scope_field_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
        if (!world_.evidence.status("composite shared source scope",
                                    ayni_memory_read_scope(source_, AYNI_MEMORY_CONTEXT,
                                                           fields.data(), fields.size()))) {
            return false;
        }
        const ayni_grant_create_v1 grant{
            sizeof(grant),
            1,
            text("memory.access"),
            world_.instance(1),
            0,
            AYNI_GRANT_PERMANENT,
            0,
            0,
            {sizeof(ayni_scope_v1), static_cast<uint32_t>(fields.size()), fields.data()}};
        const auto accepted = world_.submit(AYNI_INTENT_GRANT_CREATE, &grant);
        share_ = accepted.object.id;
        if (!world_.until("composite source shared durably",
                          [&] { return accepted.commit_seq <= world_.info().durable_seq; })) {
            return false;
        }
    }
    goal_ = create(source_);
    if (preparation == Preparation::prepared_action) {
        return world_.await_prepared([&] { return call("increment.v1"); });
    }
    return world_.until("composite first action pending",
                        [&] { return world_.service.pending() == 1; });
}
bool Composite::advance_action() {
    return world_.evidence.status("composite action executes",
                                  world_.service.execute(world_.node.kernel())) &&
           world_.evidence.status(
               "composite action receipt",
               world_.service.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE));
}
bool Composite::analysis_pending() {
    return advance_action() &&
           world_.until("composite analysis pending", [&] { return model_.pending() == 1; });
}
bool Composite::presentation_pending() {
    if (!analysis_pending()) {
        return false;
    }
    model_delivery_ = true;
    return world_.until("composite presentation pending",
                        [&] { return notification_.pending() == 1; });
}
bool Composite::finish() {
    auto &e = world_.evidence;
    if (!e.equal("capture stopped before presenting", subscription().phase,
                 AYNI_SUBSCRIPTION_STOPPED) ||
        !e.status("composite presentation executes", notification_.execute(world_.node.kernel())) ||
        !e.status("composite presentation receipt",
                  notification_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) ||
        !world_.until("composite completed durably", [&] {
            auto t = task();
            return t.lifecycle == AYNI_TASK_COMPLETED && t.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    return e.equal("one original effect", world_.service.executions(), 1) &&
           e.equal("one presentation effect", notification_.executions(), 1) &&
           e.equal("one model consumed", task().model_calls_used, 1);
}
} // namespace ayni::runtime::experiments
