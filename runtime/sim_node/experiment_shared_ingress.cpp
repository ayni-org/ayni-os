#include "experiment_shared.h"
namespace ayni::runtime::experiments {
namespace {
constexpr uint64_t pressure_tick_us = 1000000;
constexpr uint32_t pressure_steps = 64;
} // namespace
bool SharedServices::completion_pressure() {
    auto &e = world_.evidence;
    if (!verify_completion_slots(e) || !world_.grant()) {
        return false;
    }
    const auto goal = world_.create();
    if (!world_.until("counter accepted before ingress saturation",
                      [&] { return world_.service.pending() == 1; }) ||
        !e.status("external action before completion pressure",
                  world_.service.execute(world_.node.kernel()))) {
        return false;
    }
    world_.storage = World::Storage::held;
    world_.create("folder");
    if (!world_.until("storage acknowledgment outstanding",
                      [&] { return world_.node.simulation()->pending_storage() != 0; })) {
        return false;
    }
    auto event = initialized<ayni_event_v1>();
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = text("unbound.button");
    uint32_t accepted = 0;
    ayni_status_t status = AYNI_OK;
    while (accepted <= world_.config.ingress_capacity &&
           (status = ayni_post_event(world_.node.kernel(), &event)) == AYNI_OK) {
        ++accepted;
    }
    if (!e.status("full ordinary queue returns BUSY", status, AYNI_BUSY) ||
        !e.equal("ordinary queue capacity reached", accepted, world_.config.ingress_capacity) ||
        !e.equal("reserved storage completion posted through full queue",
                 world_.node.simulation()->complete_storage() > 0, 1) ||
        !e.status("reserved terminal posted through full queue",
                  world_.service.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) ||
        !e.status("duplicate terminal refused", world_.service.repeat_receipt(),
                  AYNI_INVALID_STATE)) {
        return false;
    }
    world_.storage = World::Storage::complete;
    return world_.until("completion pressure work completes durably",
                        [&] {
                            const auto done = task(goal);
                            return done.lifecycle == AYNI_TASK_COMPLETED &&
                                   done.commit_seq <= world_.info().durable_seq;
                        }) &&
           e.equal("external action executed once", world_.service.executions(), 1) &&
           e.equal("terminal returns original borrow", world_.service.pending(), 0);
}
// C-RES-004/011: sustained foreground rule events do not hide the explicit
// ordinary queue deadline, even while an old Provider still owes stop evidence.
bool SharedServices::ordinary_expiry() {
    auto &e = world_.evidence;
    if (!world_.grant(1, "test.microphone") || !world_.grant(2, "test.microphone")) {
        return false;
    }
    create_service("shared.microphone", 1);
    if (!world_.until("microphone held during pressure",
                      [&] { return microphone_.pending() == 1; })) {
        return false;
    }
    const auto waiting = create_service("shared.microphone", 2);
    if (!world_.until("ordinary resource request queued",
                      [&] { return call(waiting).wait_reason == AYNI_CALL_WAIT_OCCUPIED; })) {
        return false;
    }
    const auto began = world_.node.simulation()->now_us();
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text("shared.priority_rule");
    body.goal_type_version = 1;
    body.owner_instance_id = world_.instance(1);
    body.trigger = AYNI_TRIGGER_LOCAL_INPUT;
    body.trigger_source = text("priority.button");
    const auto high = world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
    auto event = initialized<ayni_event_v1>();
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = body.trigger_source;
    for (uint32_t at = 0; at < pressure_steps && task(waiting).lifecycle != AYNI_TASK_PAUSED;
         ++at) {
        if (!e.status("sustained foreground input",
                      ayni_post_event(world_.node.kernel(), &event))) {
            return false;
        }
        world_.node.advance_clock(pressure_tick_us);
        if (!world_.step_now()) {
            return false;
        }
    }
    const auto result = task(waiting);
    e.fact("resource", "ordinary_wait_elapsed_us", world_.node.simulation()->now_us() - began);
    return e.equal("ordinary work pauses at finite wait", result.lifecycle, AYNI_TASK_PAUSED) &&
           e.status("ordinary wait status", result.hold_status, AYNI_EXPIRED) &&
           e.equal("ordinary wait explicit reason", result.hold_reason,
                   AYNI_REASON_RESOURCE_WAIT) &&
           e.equal("foreground rule made progress", task(high).task_id != 0, 1) &&
           e.equal("waiting work never dispatched", microphone_.submissions(), 1);
}
} // namespace ayni::runtime::experiments
