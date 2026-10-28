// E1 S1/S7: the Agent-scoped view is exercised by the existing read policy;
// the authenticated management view is used only for fixture setup/diagnostics.
#include "experiment_shared.h"
#include "experiment_support.h"
#include <algorithm>
#include <ayni/host_authority.h>
#include <string>
namespace ayni::runtime::experiments {
namespace {
constexpr char private_a[] = "SYNTHETIC_PRIVATE_A_E1";
constexpr char private_b[] = "SYNTHETIC_PRIVATE_B_E1";
constexpr char profile_body[] = "SYNTHETIC_PROFILE_E1";
constexpr uint64_t voice_command = 1000;
} // namespace
uint64_t SharedServices::save_memory(uint64_t agent, const char *value,
                                     ayni_memory_namespace_t kind) {
    const ayni_memory_save_v1 body{sizeof(body),
                                   kind,
                                   0,
                                   kind == AYNI_MEMORY_PRIVATE ? world_.instance(agent) : 1,
                                   kind == AYNI_MEMORY_PRIVATE ? AYNI_PROFILE_NONE
                                                               : AYNI_PROFILE_LANGUAGE,
                                   0,
                                   text(value),
                                   nullptr,
                                   0,
                                   0,
                                   nullptr};
    const auto result = world_.submit(AYNI_INTENT_MEMORY_SAVE, &body);
    if (!world_.until("saved memory durable",
                      [&] { return result.commit_seq <= world_.info().durable_seq; })) {
        return 0;
    }
    return result.object.id;
}
uint64_t SharedServices::read_memory(uint64_t memory, uint64_t agent) {
    const auto id = std::to_string(memory);
    const auto goal =
        create(applications::normal_fixtures::GoalKind::memory_read, agent,
               {reinterpret_cast<const uint8_t *>(id.data()), static_cast<uint32_t>(id.size())});
    world_.evidence.fact("memory_read", "memory_id", memory);
    world_.evidence.fact("memory_read", "actor", world_.instance(agent));
    world_.evidence.fact("memory_read", "goal", goal);
    return goal;
}
bool SharedServices::read_outcome(uint64_t goal, ayni_status_t expected) {
    const auto lifecycle = expected == AYNI_OK ? AYNI_TASK_COMPLETED : AYNI_TASK_PAUSED;
    if (!world_.until("Agent read outcome", [&] {
            const auto current = task(goal);
            return current.lifecycle == lifecycle &&
                   current.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    const auto result = task(goal);
    if (!world_.evidence.status("Agent memory access", result.hold_status, expected)) {
        return false;
    }
    uint8_t checkpoint = 1;
    constexpr uint32_t untouched_size = UINT32_MAX;
    uint32_t bytes = untouched_size;
    const auto copied = ayni_copy_task_checkpoint(world_.node.kernel(), result.task_id, &checkpoint,
                                                  sizeof(checkpoint), &bytes);
    // C-MEM-003: an unsuccessful live Task is not exportable. BUSY leaves
    // both outputs untouched; an authorized completed read exports no body.
    return world_.evidence.equal("read produces no call output", result.logical_call_id, 0) &&
           world_.evidence.status("read checkpoint", copied,
                                  expected == AYNI_OK ? AYNI_OK : AYNI_BUSY) &&
           world_.evidence.equal("read checkpoint byte count", bytes,
                                 expected == AYNI_OK ? 0 : untouched_size) &&
           world_.evidence.equal("read cannot expose private bytes", checkpoint, 1);
}
uint64_t SharedServices::share_memory(uint64_t memory, uint64_t agent) {
    std::array<ayni_scope_field_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
    if (!world_.evidence.status(
            "exact memory read scope",
            ayni_memory_read_scope(memory, AYNI_MEMORY_CONTEXT, fields.data(), fields.size()))) {
        return 0;
    }
    const ayni_grant_create_v1 body{
        sizeof(body),
        1,
        text("memory.access"),
        world_.instance(agent),
        0,
        AYNI_GRANT_PERMANENT,
        0,
        0,
        {sizeof(ayni_scope_v1), static_cast<uint32_t>(fields.size()), fields.data()}};
    const auto result = world_.submit(AYNI_INTENT_GRANT_CREATE, &body);
    if (!world_.until("shared grant durable",
                      [&] { return result.commit_seq <= world_.info().durable_seq; })) {
        return 0;
    }
    return result.object.id;
}
bool SharedServices::trace_private_bodies_absent() const {
    const auto &lines = world_.evidence.lines();
    const bool absent = std::none_of(lines.begin(), lines.end(), [](const auto &line) {
        return line.find(private_a) != std::string::npos ||
               line.find(private_b) != std::string::npos ||
               line.find(profile_body) != std::string::npos;
    });
    return world_.evidence.equal("private/profile bodies absent from trace", absent, 1);
}
bool SharedServices::privacy_denial() {
    const auto a = save_memory(1, private_a), b = save_memory(2, private_b);
    return read_outcome(read_memory(a, 1), AYNI_OK) && read_outcome(read_memory(b, 2), AYNI_OK) &&
           read_outcome(read_memory(a, 2), AYNI_DENIED) &&
           read_outcome(read_memory(b, 1), AYNI_DENIED) &&
           world_.evidence.equal("private reads require no model",
                                 model_a_.executions() + model_b_.executions(), 0) &&
           trace_private_bodies_absent();
}
bool SharedServices::share_then_revoke() {
    const auto a = save_memory(1, private_a);
    if (!read_outcome(read_memory(a, 2), AYNI_DENIED)) {
        return false;
    }
    const auto grant = share_memory(a, 2);
    const auto shared = read_memory(a, 2);
    if (!read_outcome(shared, AYNI_OK) || !world_.revoke(grant)) {
        return false;
    }
    uint8_t old_checkpoint = 1;
    uint32_t old_bytes = 0;
    if (!world_.evidence.status("old shared context no longer exportable",
                                ayni_copy_task_checkpoint(world_.node.kernel(),
                                                          task(shared).task_id, &old_checkpoint,
                                                          sizeof(old_checkpoint), &old_bytes),
                                AYNI_EXPIRED)) {
        return false;
    }
    // C-AUTH-006/C-MEM-003: accepted revocation gates the next context before
    // background cleanup or durable retirement of the revocation is required.
    return read_outcome(read_memory(a, 2), AYNI_DENIED) && trace_private_bodies_absent();
}
bool SharedServices::profile_scope() {
    const auto profile = save_memory(1, profile_body, AYNI_MEMORY_PROFILE);
    const auto a = save_memory(1, private_a);
    if (!share_memory(profile, 1)) {
        return false;
    }
    return read_outcome(read_memory(profile, 1), AYNI_OK) &&
           read_outcome(read_memory(profile, 2), AYNI_DENIED) &&
           read_outcome(read_memory(a, 2), AYNI_DENIED) && trace_private_bodies_absent();
}
bool SharedServices::voice_boundary() {
    const auto memory = save_memory(1, private_a);
    const ayni_session_desc_v1 descriptor{sizeof(descriptor), AYNI_CHANNEL_VOICE, 1, 1, 0};
    uint64_t voice = 0;
    auto &e = world_.evidence;
    if (!e.status("open button voice session",
                  ayni_open_session(world_.node.kernel(), &descriptor, &voice))) {
        return false;
    }
    e.fact("session", "voice_channel", descriptor.channel);
    e.fact("session", "voice_id", voice);
    const auto denied = [&](ayni_intent_kind_t kind, const void *body, uint64_t command,
                            uint32_t revision) {
        auto intent = initialized<ayni_intent_v1>();
        intent.kind = kind;
        intent.session_id = voice;
        intent.command_id = command;
        intent.expected_revision = revision;
        intent.body = body;
        auto result = initialized<ayni_intent_result_v1>();
        auto error = initialized<ayni_error_v1>();
        const auto status = ayni_submit_intent(world_.node.kernel(), &intent, &result, &error);
        e.fact("voice_intent", "reason", error.reason);
        return e.status("voice cannot elevate/delete", status, AYNI_DENIED) &&
               e.equal("voice creates no result object", result.object.id, 0);
    };
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     1,
                                     text("mock.text"),
                                     world_.instance(1),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 0, nullptr}};
    const ayni_memory_target_v1 erase{sizeof(erase), 0, memory};
    if (!denied(AYNI_INTENT_GRANT_CREATE, &grant, voice_command, 0) ||
        !denied(AYNI_INTENT_MEMORY_DELETE, &erase, voice_command + 1, 1) ||
        !e.equal("voice cannot create pending authority", world_.count(AYNI_OBJECT_GRANT), 0)) {
        return false;
    }
    // Reuse both rejected identities through the real management session: a
    // denied voice request retained neither a command result nor pending Grant.
    const auto created = world_.submit(AYNI_INTENT_GRANT_CREATE, &grant, voice_command);
    auto saved = initialized<ayni_grant_info_v1>();
    if (!e.status("management grant query",
                  ayni_get_grant(world_.node.kernel(), created.object.id, &saved)) ||
        !e.equal("bound management principal", saved.owner_principal_id, descriptor.principal_id) ||
        !e.equal("bound management revision", saved.owner_binding_revision,
                 descriptor.binding_revision)) {
        return false;
    }
    if (!world_.until("management grant durable",
                      [&] { return created.commit_seq <= world_.info().durable_seq; })) {
        return false;
    }
    world_.submit(AYNI_INTENT_MEMORY_DELETE, &erase, voice_command + 1, 1);
    auto state = initialized<ayni_memory_info_v1>();
    uint32_t bytes = 0;
    return e.status("deleted memory metadata",
                    ayni_get_memory(world_.node.kernel(), world_.owner_session(), memory, &state,
                                    nullptr, 0, &bytes)) &&
           e.equal("management deletion effective", state.state != AYNI_MEMORY_LIVE, 1) &&
           e.status("close voice session", ayni_close_session(world_.node.kernel(), voice)) &&
           trace_private_bodies_absent();
}
} // namespace ayni::runtime::experiments
