#include "experiment_shared.h"
#include <array>
#include <cstring>
namespace ayni::runtime::experiments {
namespace {
using applications::assistant::GoalKind;
constexpr uint64_t periodic_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US;
constexpr char source_body[] = "SYNTHETIC_SOURCE_E1";
constexpr char edited_body[] = "SYNTHETIC_USER_NEW_VERSION_E1";
} // namespace
bool SharedServices::register_assistant(ayni_kernel_t *kernel) {
    auto &e = world_.evidence;
    const ayni_resource_desc_v1 resource{sizeof(resource),
                                         AYNI_RESOURCE_SHARED,
                                         text("notification.window"),
                                         1,
                                         0,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
    const auto provider = notification_.describe();
    const ayni_capability_desc_v1 authority{
        sizeof(authority), 1, text("notify.present"), 0, 0, nullptr};
    const ayni_binding_v1 present{sizeof(present),
                                  1,
                                  text("notify"),
                                  authority.capability_id,
                                  1,
                                  1,
                                  provider.provider_id,
                                  provider.endpoint_id,
                                  {}};
    const auto model = offers_->a;
    const ayni_binding_v1 summary{
        sizeof(summary),   1, text("summary"), text("mock.text"), 1, 1, model.provider_id,
        model.endpoint_id, {}};
    assistant_.emplace();
    applications::assistant::PolicyConfig config;
    config.model_capability = text("mock.text");
    config.present_capability = authority.capability_id;
    config.health_capability = text("test.increment");
    config.observe_capability = text("mock.observe");
    const auto clock = world_.node.clock();
    config.clock_context = clock.context;
    config.utc_now = clock.utc_now;
    return e.status("register presentation window", ayni_register_resource(kernel, &resource)) &&
           e.status("presentation window online",
                    ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE)) &&
           e.status("register presentation authority",
                    ayni_register_capability(kernel, &authority)) &&
           e.status("register presentation contract",
                    ayni_register_call_capability(kernel, provider.capabilities)) &&
           e.status("register presentation service", ayni_register_provider(kernel, &provider)) &&
           e.status("register presentation binding", ayni_register_binding(kernel, &present)) &&
           e.status("register summary binding", ayni_register_binding(kernel, &summary)) &&
           e.status("register actual assistant workflows",
                    assistant_->register_all(kernel, config));
}
uint64_t SharedServices::organize(uint64_t source, uint64_t agent) {
    applications::assistant::Parameters p;
    p.kind = GoalKind::memory_organize;
    p.sources = {source};
    applications::assistant::Encoded bytes;
    auto body = initialized<ayni_goal_create_v1>();
    if (!world_.evidence.status("prepare actual organize Goal",
                                applications::assistant::prepare_goal(p, world_.instance(agent),
                                                                      periodic_us, nullptr, bytes,
                                                                      body))) {
        return 0;
    }
    const auto goal = world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
    world_.node.advance_clock(periodic_us);
    return goal;
}
// C-PROD-030 / C-COMMIT-012: let an actual old Model response arrive after its
// source basis changes; management reads only prove the surviving user record.
bool SharedServices::invalidate_summary(SourceChange change) {
    auto &e = world_.evidence;
    const auto source = save_memory(1, source_body);
    if (!source || !world_.grant(1, "mock.text") || !world_.grant(2, "mock.text") ||
        !e.status("late model completion after cancel",
                  model_a_.set_cancellation(fixtures::ModelProbe::Cancellation::late_success))) {
        return false;
    }
    uint64_t completed_summary_task = 0;
    if (change == SourceChange::erase) {
        const auto previous = organize(source);
        if (!world_.until("existing derived summary completed durably", [&] {
                const auto done = task(previous);
                return done.lifecycle == AYNI_TASK_COMPLETED &&
                       done.commit_seq <= world_.info().durable_seq;
            })) {
            return false;
        }
        completed_summary_task = task(previous).task_id;
        const ayni_memory_search_v1 query{
            sizeof(query), AYNI_MEMORY_PRIVATE, world_.instance(1), 0, {}, {}};
        std::array<ayni_memory_info_v1, 16> rows{};
        uint32_t count = 0;
        if (!e.status("existing derived summary visible",
                      ayni_search_memory(world_.node.kernel(), world_.owner_session(), &query,
                                         rows.data(), rows.size(), &count)) ||
            !e.equal("source and derived summary exist", count, 2) || !cancel(previous)) {
            return false;
        }
    }
    const auto executions_before = model_a_.executions();
    uint64_t share = 0;
    if (change == SourceChange::revoke) {
        std::array<ayni_scope_field_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
        if (!e.status("organize share scope",
                      ayni_memory_read_scope(source, AYNI_MEMORY_CONTEXT, fields.data(),
                                             fields.size()))) {
            return false;
        }
        const ayni_grant_create_v1 body{
            sizeof(body),
            1,
            text("memory.access"),
            world_.instance(2),
            0,
            AYNI_GRANT_PERMANENT,
            0,
            0,
            {sizeof(ayni_scope_v1), static_cast<uint32_t>(fields.size()), fields.data()}};
        share = world_.submit(AYNI_INTENT_GRANT_CREATE, &body).object.id;
    }
    uint64_t goal = 0;
    if (change == SourceChange::revoke) {
        const auto id = std::to_string(source);
        auto body = initialized<ayni_goal_create_v1>();
        body.goal_type_id = text("shared.context_summary");
        body.goal_type_version = 1;
        body.owner_instance_id = world_.instance(2);
        body.trigger = AYNI_TRIGGER_ONCE;
        body.parameters = {reinterpret_cast<const uint8_t *>(id.data()),
                           static_cast<uint32_t>(id.size())};
        goal = world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
    } else {
        goal = organize(source);
    }
    if (!goal || !world_.until("organize has read source and dispatched model",
                               [&] { return model_a_.pending() == 1; })) {
        return false;
    }
    const auto original = call(goal);
    const auto original_task = task(goal);
    if (change == SourceChange::edit) {
        const ayni_memory_save_v1 body{sizeof(body),
                                       AYNI_MEMORY_PRIVATE,
                                       source,
                                       world_.instance(1),
                                       AYNI_PROFILE_NONE,
                                       0,
                                       text(edited_body),
                                       nullptr,
                                       0,
                                       0,
                                       nullptr};
        world_.submit(AYNI_INTENT_MEMORY_SAVE, &body, 0, 1);
        // A stale writer cannot use the pre-edit CAS even through management.
        world_.submit(AYNI_INTENT_MEMORY_SAVE, &body, 0, 1, AYNI_REVISION_CONFLICT);
    } else if (change == SourceChange::revoke) {
        if (!world_.revoke(share)) {
            return false;
        }
    } else {
        const ayni_memory_target_v1 body{sizeof(body), 0, source};
        world_.submit(AYNI_INTENT_MEMORY_DELETE, &body, 0, 1);
    }
    if (!e.ok()) {
        return false;
    }
    auto privacy = initialized<ayni_privacy_info_v1>();
    if (change == SourceChange::restart_cleanup) {
        if (!e.status(
                "privacy cleanup started",
                ayni_get_privacy_status(world_.node.kernel(), world_.owner_session(), &privacy)) ||
            !e.equal("cleanup unfinished at interruption", privacy.phase != AYNI_PRIVACY_COMPLETE,
                     1) ||
            !world_.until("delete accepted durably before restart",
                          [&] {
                              auto info = initialized<ayni_memory_info_v1>();
                              uint32_t size = 0;
                              return ayni_get_memory(world_.node.kernel(), world_.owner_session(),
                                                     source, &info, nullptr, 0, &size) == AYNI_OK &&
                                     info.commit_seq <= world_.info().durable_seq;
                          }) ||
            !world_.crash(hosts::sim::CutAppend::lost) || !world_.start()) {
            return false;
        }
    } else {
        if (!world_.until("old model reply returned", [&] { return model_a_.pending() == 0; }) ||
            !e.equal("old response actually executed", model_a_.executions(),
                     executions_before + 1)) {
            return false;
        }
    }
    if (change == SourceChange::revoke) {
        if (!world_.until("revoked shared context held", [&] {
                const auto current = task(goal);
                return current.hold_status == AYNI_EXPIRED &&
                       current.commit_seq <= world_.info().durable_seq;
            })) {
            return false;
        }
    } else if (!world_.until("privacy sweep completes", [&] {
                   return ayni_get_privacy_status(world_.node.kernel(), world_.owner_session(),
                                                  &privacy) == AYNI_OK &&
                          privacy.phase == AYNI_PRIVACY_COMPLETE;
               })) {
        return false;
    }
    auto metadata = initialized<ayni_memory_info_v1>();
    std::array<char, 128> body{};
    uint32_t size = 0;
    if (change == SourceChange::edit) {
        if (!e.status("read preserved user revision",
                      ayni_get_memory(world_.node.kernel(), world_.owner_session(), source,
                                      &metadata, body.data(), body.size(), &size)) ||
            !e.equal("user revision preserved", metadata.revision, 2) ||
            !e.equal("new user body preserved",
                     size == sizeof(edited_body) - 1 &&
                         std::memcmp(body.data(), edited_body, size) == 0,
                     1)) {
            return false;
        }
    } else if (change != SourceChange::revoke) {
        if (!e.status("deleted source stays unreadable",
                      ayni_get_memory(world_.node.kernel(), world_.owner_session(), source,
                                      &metadata, body.data(), body.size(), &size),
                      AYNI_NOT_FOUND)) {
            return false;
        }
    }
    if (completed_summary_task != 0) {
        uint8_t body_byte = 1;
        uint32_t bytes = UINT32_MAX;
        if (!e.status("completed derived context invalidated",
                      ayni_copy_task_checkpoint(world_.node.kernel(), completed_summary_task,
                                                &body_byte, 1, &bytes),
                      AYNI_EXPIRED)) {
            return false;
        }
    }
    uint8_t checkpoint = 1;
    size = UINT32_MAX;
    const auto exported = ayni_copy_task_checkpoint(world_.node.kernel(), original_task.task_id,
                                                    &checkpoint, 1, &size);
    uint32_t memories = 0;
    for (uint64_t agent : {UINT64_C(1), UINT64_C(2)}) {
        const ayni_memory_search_v1 query{
            sizeof(query), AYNI_MEMORY_PRIVATE, world_.instance(agent), 0, {}, {}};
        std::array<ayni_memory_info_v1, 16> records{};
        uint32_t count = 0;
        if (!e.status("search remaining private records",
                      ayni_search_memory(world_.node.kernel(), world_.owner_session(), &query,
                                         records.data(), records.size(), &count))) {
            return false;
        }
        memories += count;
        for (uint32_t at = 0; at < count; ++at) {
            if (!e.equal("no stale derived candidate", records[at].source, AYNI_MEMORY_USER)) {
                return false;
            }
        }
    }
    const auto expected = change == SourceChange::edit || change == SourceChange::revoke ? 1U : 0U;
    return e.status("invalidated Task export refused", exported, AYNI_BUSY) &&
           e.equal("only surviving user memory remains", memories, expected) &&
           e.equal("original logical call retained", call(goal).logical_call_id,
                   original.logical_call_id) &&
           trace_private_bodies_absent();
}
} // namespace ayni::runtime::experiments
