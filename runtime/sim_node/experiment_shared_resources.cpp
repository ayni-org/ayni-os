#include "experiment_shared.h"
#include <ayni/observations.h>
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t camera_capacity = 2;
constexpr uint64_t timeout_delay_us = 1500000;
constexpr uint64_t timeout_us = 1000000;
} // namespace
bool SharedServices::register_resources(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    if (!camera_offer_) {
        providers::mock::SubscriptionConfig config;
        const auto clock = world_.node.clock();
        config.clock_context = clock.context;
        config.now_us = clock.now_us;
        if (!e.status("configure camera", camera_.configure(config))) {
            return false;
        }
        ayni_provider_v1 offer{};
        if (!e.status("describe camera", camera_.describe(offer))) {
            return false;
        }
        camera_offer_ = offer;
    }
    for (const char *id : {"microphone.slot", "test.actuator", "camera.slot"}) {
        const bool camera = std::string(id) == "camera.slot";
        const ayni_resource_desc_v1 value{sizeof(value),
                                          camera ? AYNI_RESOURCE_SHARED : AYNI_RESOURCE_EXCLUSIVE,
                                          text(id),
                                          camera ? camera_capacity : 1U,
                                          0,
                                          0,
                                          0,
                                          nullptr,
                                          {}};
        if (!e.status("register shared resource", ayni_register_resource(kernel, &value)) ||
            !e.status("shared resource online",
                      ayni_set_resource_health(kernel, value.resource_id, AYNI_RESOURCE_ONLINE))) {
            return false;
        }
    }
    auto camera = *camera_offer_;
    camera.ctx = this;
    camera.submit = camera_submit;
    camera.cancel = [](void *ctx, ayni_attempt_ref_v1 attempt) {
        const auto &offer = *static_cast<SharedServices *>(ctx)->camera_offer_;
        return offer.cancel(offer.ctx, attempt);
    };
    camera.renew = [](void *ctx, const ayni_subscription_renew_v1 *renew) {
        auto &self = *static_cast<SharedServices *>(ctx);
        const auto &offer = *self.camera_offer_;
        const auto status = offer.renew(offer.ctx, renew);
        if (status != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
            ++self.camera_renewals_;
        }
        return status;
    };
    const auto mic = microphone_.describe(), group = group_.describe();
    const ayni_provider_v1 providers[] = {mic, group, camera};
    const char *const bindings[] = {"microphone", "group", "observe"};
    for (uint32_t at = 0; at < sizeof(providers) / sizeof(providers[0]); ++at) {
        const auto &offer = providers[at];
        const ayni_capability_desc_v1 authority{
            sizeof(authority), 1, offer.capabilities->capability_id, 0, 0, nullptr};
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text(bindings[at]),
                                      authority.capability_id,
                                      1,
                                      1,
                                      offer.provider_id,
                                      offer.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        if (!e.status("register service authority", ayni_register_capability(kernel, &authority)) ||
            !e.status("register service contract",
                      ayni_register_call_capability(kernel, offer.capabilities)) ||
            !e.status("register service provider", ayni_register_provider(kernel, &offer)) ||
            !e.status("register service binding", ayni_register_binding(kernel, &binding))) {
            return false;
        }
    }
    ayni_observation_source_v1 source{};
    return e.status("describe camera source", camera_.describe_source(source)) &&
           e.status("register camera source", ayni_register_observation_source(kernel, &source)) &&
           e.status("register service workflows", services_policy_.register_all(kernel));
}
ayni_submit_result_t SharedServices::camera_submit(void *ctx,
                                                   const ayni_provider_request_v1 *request,
                                                   ayni_provider_completion_t *completion) {
    auto &self = *static_cast<SharedServices *>(ctx);
    const auto &offer = *self.camera_offer_;
    const auto status = offer.submit(offer.ctx, request, completion);
    if (status != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        self.last_opening_ = *request->subscription;
    }
    return status;
}
uint64_t SharedServices::create_service(const char *type, uint64_t agent) {
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text(type);
    body.goal_type_version = 1;
    body.owner_instance_id = world_.instance(agent);
    body.trigger = AYNI_TRIGGER_ONCE;
    return world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
}
ayni_resource_info_v1 SharedServices::resource(const char *id) const {
    auto out = initialized<ayni_resource_info_v1>();
    const auto status = ayni_get_resource(world_.node.kernel(), text(id), &out);
    if (status != AYNI_OK) {
        world_.evidence.status("get service resource", status);
    }
    return out;
}
ayni_subscription_info_v1 SharedServices::subscription(uint64_t goal) const {
    auto out = initialized<ayni_subscription_info_v1>();
    const auto current = call(goal);
    if (current.logical_call_id) {
        const auto status =
            ayni_get_subscription(world_.node.kernel(), current.logical_call_id, &out);
        if (status != AYNI_OK) {
            world_.evidence.status("get camera lease", status);
        }
    }
    return out;
}
bool SharedServices::cancel(uint64_t goal) {
    return change_goal(goal, AYNI_INTENT_GOAL_CANCEL);
}
bool SharedServices::change_goal(uint64_t goal, ayni_intent_kind_t kind) {
    auto info = initialized<ayni_goal_info_v1>();
    if (!world_.evidence.status("read cancelled Goal",
                                ayni_get_goal(world_.node.kernel(), goal, &info))) {
        return false;
    }
    const ayni_goal_target_v1 body{sizeof(body), 0, goal};
    world_.submit(kind, &body, 0, info.revision);
    return world_.evidence.ok();
}
bool SharedServices::open_camera(uint64_t &goal, uint64_t agent) {
    if (!world_.grant(agent, "mock.observe")) {
        return false;
    }
    goal = create_service("shared.observe", agent);
    return world_.until("camera active",
                        [&] { return subscription(goal).phase == AYNI_SUBSCRIPTION_ACTIVE; });
}
// C-RES-004/009/010: independent shared leases coexist with a FIFO exclusive
// holder. Explicit receipts separate release from merely requesting cancellation.
bool SharedServices::resources_parallel() {
    auto &e = world_.evidence;
    if (!world_.grant(1, "test.microphone") || !world_.grant(2, "test.microphone")) {
        return false;
    }
    uint64_t ca = 0, cb = 0;
    if (!open_camera(ca, 1) || !open_camera(cb, 2)) {
        return false;
    }
    const auto a = create_service("shared.microphone", 1);
    if (!world_.until("A holds microphone", [&] { return microphone_.pending() == 1; })) {
        return false;
    }
    const auto b = create_service("shared.microphone", 2);
    if (!world_.until("B queues microphone",
                      [&] { return call(b).wait_reason == AYNI_CALL_WAIT_OCCUPIED; })) {
        return false;
    }
    auto queue = initialized<ayni_resource_queue_info_v1>();
    if (!e.status("resource FIFO diagnostics",
                  ayni_get_resource_queue(world_.node.kernel(), &queue)) ||
        !e.equal("one microphone holder", resource("microphone.slot").used, 1) ||
        !e.equal("two camera leases in parallel", resource("camera.slot").used, camera_capacity) ||
        !e.equal("second microphone not dispatched", microphone_.submissions(), 1) ||
        !e.equal("one FIFO waiter", queue.waiting, 1)) {
        return false;
    }
    const auto queued_at = world_.node.simulation()->now_us();
    if (!e.status("microphone A completed",
                  microphone_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) ||
        !world_.until("B admitted after release", [&] { return microphone_.submissions() == 2; }) ||
        !e.equal("A completed once", task(a).lifecycle, AYNI_TASK_COMPLETED) ||
        !e.equal("microphone still exclusive", resource("microphone.slot").used, 1)) {
        return false;
    }
    e.fact("resource", "second_admission_us", world_.node.simulation()->now_us());
    e.fact("resource", "observed_wait_us", world_.node.simulation()->now_us() - queued_at);
    if (!e.status("longest wait diagnostic",
                  ayni_get_resource_queue(world_.node.kernel(), &queue))) {
        return false;
    }
    e.fact("resource", "longest_wait_us", queue.longest_wait_us);
    return e.status("microphone B completed",
                    microphone_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) &&
           cancel(ca) && cancel(cb) && world_.until("all resources returned", [&] {
               return task(b).lifecycle == AYNI_TASK_COMPLETED && camera_.pending() == 0 &&
                      resource("camera.slot").used == 0 && resource("microphone.slot").used == 0;
           });
}
bool SharedServices::resource_group() {
    auto &e = world_.evidence;
    if (!world_.grant(1, "test.microphone") || !world_.grant(2, "test.group")) {
        return false;
    }
    create_service("shared.microphone", 1);
    if (!world_.until("microphone busy", [&] { return microphone_.pending() == 1; })) {
        return false;
    }
    const auto grouped = create_service("shared.group", 2);
    if (!world_.until("group waits atomically",
                      [&] { return call(grouped).wait_reason == AYNI_CALL_WAIT_OCCUPIED; })) {
        return false;
    }
    return e.equal("actuator never partially acquired", resource("test.actuator").used, 0) &&
           e.equal("group not dispatched", group_.submissions(), 0) &&
           e.status("release microphone",
                    microphone_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) &&
           world_.until("whole group admitted", [&] { return group_.pending() == 1; }) &&
           e.equal("group owns microphone", resource("microphone.slot").used, 1) &&
           e.equal("group owns actuator", resource("test.actuator").used, 1) &&
           e.status("group confirmed complete",
                    group_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) &&
           world_.until("whole group released", [&] {
               return task(grouped).lifecycle == AYNI_TASK_COMPLETED &&
                      resource("microphone.slot").used == 0 && resource("test.actuator").used == 0;
           });
}
bool SharedServices::timeout_cancel() {
    auto &e = world_.evidence;
    if (!e.status("set delayed Mock", model_a_.set_delay(timeout_delay_us)) ||
        !world_.grant(1, "mock.text") || !world_.grant(2, "mock.text")) {
        return false;
    }
    const auto a = create(applications::normal_fixtures::GoalKind::model, 1);
    if (!world_.until("delayed model accepted", [&] { return model_a_.pending() == 1; })) {
        return false;
    }
    const auto dispatch_at = world_.node.simulation()->now_us();
    const auto b = create_chat(2);
    if (!world_.until("waiting model queued", [&] { return call(b).phase == AYNI_CALL_QUEUED; }) ||
        !cancel(b)) {
        return false;
    }
    if (!world_.until("timeout returns borrow", [&] {
            return call(a).phase == AYNI_CALL_FINISHED && model_a_.pending() == 0;
        })) {
        return false;
    }
    const auto terminal = call(a);
    e.fact("provider", "timeout_elapsed_us", world_.node.simulation()->now_us() - dispatch_at);
    return e.equal("timeout elapsed before delayed success",
                   world_.node.simulation()->now_us() - dispatch_at >= timeout_us, 1) &&
           e.equal("timed out call cancelled", terminal.outcome, AYNI_CALL_CANCELLED) &&
           e.equal("cancelled waiter never dispatched", model_a_.executions(), 0) &&
           e.equal("model resource released", model_slot().used, 0);
}
bool SharedServices::subscription_close() {
    auto &e = world_.evidence;
    uint64_t first = 0;
    if (!open_camera(first) || !world_.until("camera sample accepted", [&] {
            auto sample = initialized<ayni_observation_info_v1>();
            return ayni_get_subscription_observation(
                       world_.node.kernel(), call(first).logical_call_id, &sample) == AYNI_OK &&
                   sample.usable;
        })) {
        return false;
    }
    const auto opening = *last_opening_;
    auto sample = initialized<ayni_observation_info_v1>();
    if (!e.status("read old camera sample",
                  ayni_get_subscription_observation(world_.node.kernel(),
                                                    call(first).logical_call_id, &sample)) ||
        !change_goal(first, AYNI_INTENT_GOAL_PAUSE) || !world_.until("camera stop confirmed", [&] {
            return subscription(first).phase == AYNI_SUBSCRIPTION_STOPPED && camera_.pending() == 0;
        })) {
        return false;
    }
    auto tail = initialized<ayni_subscription_observation_v1>();
    tail.subscription = opening.subscription;
    tail.sample.struct_size = sizeof(tail.sample);
    tail.sample.source_version = sample.reference.source_version;
    tail.sample.source_id = text("mock.camera");
    tail.sample.generation = sample.reference.generation;
    tail.sample.sequence = sample.reference.sequence + 1;
    tail.sample.captured_at_us = world_.node.simulation()->now_us();
    tail.sample.valid_for_us = 1;
    tail.sample.quality = AYNI_OBSERVATION_GOOD;
    tail.sample.value = sample.value;
    if (!change_goal(first, AYNI_INTENT_GOAL_RESUME) ||
        !world_.until("same subscription reopens", [&] {
            return subscription(first).phase == AYNI_SUBSCRIPTION_ACTIVE &&
                   subscription(first).subscription.generation != opening.subscription.generation;
        })) {
        return false;
    }
    return e.status("closed generation tail refused",
                    ayni_subscription_post_observation(opening.sink, &tail), AYNI_STALE) &&
           e.equal("stable subscription identity", subscription(first).subscription.subscription_id,
                   opening.subscription.subscription_id) &&
           e.equal("one current camera lease", resource("camera.slot").used, 1) && cancel(first) &&
           world_.until("reopened camera also stopped", [&] {
               return camera_.pending() == 0 && resource("camera.slot").used == 0;
           });
}
bool SharedServices::subscription_revoke() {
    auto &e = world_.evidence;
    uint64_t goal = 0;
    if (!open_camera(goal)) {
        return false;
    }
    const auto original = subscription(goal);
    if (!world_.until(
            "camera renewal confirmed",
            [&] { return subscription(goal).lease_sequence > original.lease_sequence; }) ||
        !e.equal("renewal preserves generation", subscription(goal).subscription.generation,
                 original.subscription.generation) ||
        !e.equal("renewal extends finite deadline",
                 subscription(goal).valid_until_us > original.valid_until_us, 1)) {
        return false;
    }
    const auto accepted_renewals = camera_renewals_;
    // The World retains the most recently accepted Grant for this exact opening.
    if (!world_.revoke()) {
        return false;
    }
    const auto before = subscription(goal).lease_sequence;
    if (!world_.until("revoked subscription stops", [&] {
            return camera_.pending() == 0 && resource("camera.slot").used == 0;
        })) {
        return false;
    }
    return e.equal("no renewal request after revoke", camera_renewals_, accepted_renewals) &&
           e.equal("no renewal after revoke", subscription(goal).lease_sequence, before) &&
           e.equal("media objects reclaimed", camera_.media_store()->occupied(), 0) &&
           e.equal("revoked lease not active", subscription(goal).phase != AYNI_SUBSCRIPTION_ACTIVE,
                   1);
}
} // namespace ayni::runtime::experiments
