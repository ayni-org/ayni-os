#include "multinode_transport.h"
#include <ayni/applications/normal_fixtures.h>
#include <ayni/ayni.hpp>
#include <cstring>
namespace ayni::runtime::multinode {
namespace {
constexpr uint32_t text_limit = 512, timeout_ms = 10000, call_capacity = 8;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
bool same(ayni_string_v1 s, const char *expected) {
    return s.data && s.size == std::strlen(expected) && std::memcmp(s.data, expected, s.size) == 0;
}
bool destination(const ayni_scope_v1 &arguments) {
    using namespace applications::normal_fixtures;
    if (arguments.field_count != 1 || !arguments.fields) {
        return false;
    }
    const auto &field = arguments.fields[0];
    return field.field_id == remote_destination_field &&
           field.allowed.kind == AYNI_AUTHORITY_IDENTIFIER &&
           field.allowed.values.tokens.member_count == 1 && field.allowed.values.tokens.members &&
           same(field.allowed.values.tokens.members[0], remote_destination);
}
} // namespace
FakeTransport::FakeTransport() {
    resource_ = {text("counter.slot"), 1, 0};
    capability_ = initialized<ayni_call_capability_v1>();
    capability_.version = 1;
    capability_.capability_id = text("test.increment");
    capability_.contract = {sizeof(ayni_call_contract_v1),
                            AYNI_CALL_ACTION,
                            AYNI_CALL_TOOL,
                            AYNI_CALL_EFFECT_EGRESS,
                            AYNI_CALL_NOT_IDEMPOTENT,
                            AYNI_CALL_QUERY_NONE,
                            AYNI_CALL_CANCEL_NONE,
                            0,
                            timeout_ms,
                            AYNI_CALL_INPUT_FIELDS_TEXT,
                            AYNI_CALL_RESULT_TEXT,
                            text_limit,
                            0,
                            1,
                            &resource_,
                            nullptr};
}
ayni_provider_v1 FakeTransport::describe() {
    auto out = initialized<ayni_provider_v1>();
    out.version = 1;
    out.provider_id = text("sim.transport");
    out.endpoint_id = out.destination_id = text(applications::normal_fixtures::remote_destination);
    out.ctx = this;
    out.capability_count = 1;
    out.capabilities = &capability_;
    out.submit = submit;
    return out;
}
ayni_status_t FakeTransport::connect(Route route) {
    if (connected_) {
        return AYNI_INVALID_STATE;
    }
    if (!route.caller || !route.receiver || route.caller == route.receiver ||
        !route.caller_instance || !route.service_instance || !route.receiver_management_session ||
        !route.service_goal_type || !route.now_us || !route.invoke_delay_us ||
        !route.query_delay_us) {
        return AYNI_INVALID_ARGUMENT;
    }
    route_ = route;
    command_ = route.first_command;
    connected_ = true;
    return AYNI_OK;
}
ayni_submit_result_t FakeTransport::submit(void *context, const ayni_provider_request_v1 *r,
                                           ayni_provider_completion_t *completion) {
    auto &self = *static_cast<FakeTransport *>(context);
    if (!self.connected_ || !r || !completion || r->struct_size != sizeof(*r) ||
        !r->logical_call_id || !r->operation_id || r->capability_version != 1 ||
        !same(r->capability_id, "test.increment") ||
        !same(r->endpoint_id, applications::normal_fixtures::remote_destination) || r->text.size ||
        !destination(r->arguments) || r->execution_permit || r->subscription) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    // Closed runs retain every identity. Repeated submit is refused; this is
    // not a wire retry cache or a persistent low-watermark implementation.
    for (const auto &entry : self.entries_) {
        if (entry.phase != Phase::empty && entry.operation == r->operation_id) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
    }
    const auto now = self.route_.now_us(self.route_.clock_context);
    if (now > UINT64_MAX - self.route_.invoke_delay_us) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    for (auto &entry : self.entries_) {
        if (entry.phase == Phase::empty) {
            entry.phase = Phase::transit;
            entry.call = r->logical_call_id;
            entry.operation = r->operation_id;
            entry.attempt = r->attempt;
            entry.completion = completion;
            entry.due_us = now + self.route_.invoke_delay_us;
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_status_t FakeTransport::receive(Entry &entry) {
    auto call = initialized<ayni_call_info_v1>();
    auto info = initialized<ayni_kernel_info_v1>();
    auto status = ayni_get_call(route_.caller, entry.call, &call);
    if (status == AYNI_OK) {
        status = ayni_get_kernel_info(route_.caller, &info);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (call.owner_instance_id != route_.caller_instance || call.operation_id != entry.operation) {
        return AYNI_DENIED;
    }
    if (!call.requires_seq || call.requires_seq > info.durable_seq) {
        return AYNI_NOT_READY;
    }
    if (call.cancel_requested) {
        entry.terminal = initialized<ayni_provider_result_v1>();
        entry.terminal.attempt = entry.attempt;
        entry.terminal.outcome = AYNI_CALL_CANCELLED;
        entry.terminal.status = AYNI_OK;
        entry.terminal.effect = AYNI_CALL_EFFECT_NONE;
        entry.phase = Phase::terminal;
        return AYNI_OK;
    }
    if (command_ == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_version = 1;
    body.goal_type_id = text(route_.service_goal_type);
    body.owner_instance_id = route_.service_instance;
    body.trigger = AYNI_TRIGGER_ONCE;
    const ayni_intent_v1 intent{sizeof(intent),
                                AYNI_INTENT_GOAL_CREATE,
                                command_ + 1,
                                route_.receiver_management_session,
                                0,
                                0,
                                &body};
    auto result = initialized<ayni_intent_result_v1>();
    auto error = initialized<ayni_error_v1>();
    status = ayni_submit_intent(route_.receiver, &intent, &result, &error);
    if (status == AYNI_BUSY || status == AYNI_NOT_READY) {
        return AYNI_NOT_READY;
    }
    if (status != AYNI_OK) {
        return status;
    }
    ++command_;
    entry.caller_task = call.task_id;
    entry.goal = result.object.id;
    entry.acceptance_seq = result.commit_seq;
    entry.phase = Phase::accepted;
    ++received_;
    return AYNI_OK;
}
ayni_status_t FakeTransport::observe(const Entry &entry, Reply &reply,
                                     ayni_provider_result_v1 &terminal) const {
    if (!entry.goal) {
        return AYNI_NOT_FOUND;
    }
    auto goal = initialized<ayni_goal_info_v1>();
    auto info = initialized<ayni_kernel_info_v1>();
    auto status = ayni_get_goal(route_.receiver, entry.goal, &goal);
    if (status == AYNI_OK) {
        status = ayni_get_kernel_info(route_.receiver, &info);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (goal.owner_instance_id != route_.service_instance) {
        return AYNI_DENIED;
    }
    reply = {entry.operation,
             entry.caller_task,
             route_.caller_instance,
             entry.goal,
             0,
             route_.service_instance,
             entry.acceptance_seq,
             info.durable_seq,
             0,
             0,
             false};
    if (!goal.last_task_id) {
        return AYNI_OK;
    }
    auto task = initialized<ayni_task_info_v1>();
    status = ayni_get_task(route_.receiver, goal.last_task_id, &task);
    if (status != AYNI_OK) {
        return status;
    }
    reply.service_task = task.task_id;
    reply.task_state = task.lifecycle;
    if (task.owner_instance_id != route_.service_instance) {
        return AYNI_DENIED;
    }
    // The closed normal profile cannot infer an effect from a different
    // terminal Task state. Retain the accepted borrow and surface uncertainty.
    if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_CANCELLED ||
        task.lifecycle == AYNI_TASK_EXPIRED) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (task.lifecycle != AYNI_TASK_COMPLETED) {
        return AYNI_OK;
    }
    std::array<uint64_t, call_capacity> ids{};
    uint32_t count = 0;
    status = ayni_list_objects(route_.receiver, AYNI_OBJECT_CALL, ids.data(),
                               static_cast<uint32_t>(ids.size()), &count);
    if (status != AYNI_OK) {
        return status;
    }
    for (uint32_t index = 0; index < count; ++index) {
        auto call = initialized<ayni_call_info_v1>();
        status = ayni_get_call(route_.receiver, ids[index], &call);
        if (status != AYNI_OK) {
            return status;
        }
        if (call.task_id != task.task_id) {
            continue;
        }
        if (!call.requires_seq || call.requires_seq > info.durable_seq ||
            goal.commit_seq > info.durable_seq || task.commit_seq > info.durable_seq) {
            return AYNI_OK;
        }
        if (call.outcome != AYNI_CALL_COMPLETED || call.status != AYNI_OK ||
            call.effect != AYNI_CALL_EFFECT_COMPLETE) {
            return AYNI_RESULT_UNKNOWN;
        }
        reply.completed = true;
        reply.effect = call.effect;
        terminal = initialized<ayni_provider_result_v1>();
        terminal.attempt = entry.attempt;
        terminal.outcome = AYNI_CALL_COMPLETED;
        terminal.status = call.status;
        terminal.effect = call.effect;
        return AYNI_OK;
    }
    return AYNI_NOT_FOUND;
}
ayni_status_t FakeTransport::poll(Entry &entry) {
    if (entry.phase == Phase::accepted) {
        Reply reply{};
        auto status = observe(entry, reply, entry.terminal);
        if (status != AYNI_OK) {
            return status;
        }
        if (reply.completed) {
            entry.phase = Phase::terminal;
        }
    }
    if (entry.phase == Phase::terminal) {
        const auto status = ayni_provider_post(entry.completion, &entry.terminal);
        if (status == AYNI_BUSY) {
            return AYNI_OK;
        }
        if (status != AYNI_OK) {
            return status;
        }
        entry.completion = nullptr;
        entry.phase = Phase::delivered;
        ++delivered_;
    }
    return AYNI_OK;
}
ayni_status_t FakeTransport::advance() {
    if (!connected_) {
        return AYNI_INVALID_STATE;
    }
    const auto now = route_.now_us(route_.clock_context);
    for (auto &entry : entries_) {
        if (entry.phase == Phase::transit && now >= entry.due_us) {
            const auto status = receive(entry);
            if (status != AYNI_OK && status != AYNI_NOT_READY) {
                return status;
            }
        }
        const auto status = poll(entry);
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (query_.phase == QueryPhase::transit && now >= query_.due_us) {
        for (const auto &entry : entries_) {
            if (entry.phase != Phase::empty && entry.operation == query_.operation) {
                ayni_provider_result_v1 ignored{};
                query_.status = observe(entry, query_.reply, ignored);
                query_.phase = QueryPhase::ready;
                return AYNI_OK;
            }
        }
        query_.status = AYNI_NOT_FOUND;
        query_.phase = QueryPhase::ready;
    }
    return AYNI_OK;
}
ayni_status_t FakeTransport::query(uint64_t operation) {
    if (!connected_ || !operation) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (query_.phase != QueryPhase::empty) {
        return AYNI_BUSY;
    }
    const auto now = route_.now_us(route_.clock_context);
    if (now > UINT64_MAX - route_.query_delay_us) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    query_ = {QueryPhase::transit, operation, now + route_.query_delay_us, AYNI_NOT_READY, {}};
    return AYNI_OK;
}
ayni_status_t FakeTransport::take_reply(Reply &out) {
    if (query_.phase != QueryPhase::ready) {
        return AYNI_NOT_READY;
    }
    const auto status = query_.status;
    if (status == AYNI_OK) {
        out = query_.reply;
    }
    query_ = {};
    return status;
}
bool FakeTransport::pending() const {
    for (const auto &entry : entries_) {
        if (entry.completion) {
            return true;
        }
    }
    return query_.phase != QueryPhase::empty;
}
void FakeTransport::cancel_query() {
    query_ = {};
}
uint64_t FakeTransport::last_operation() const {
    uint64_t result = 0;
    for (const auto &entry : entries_) {
        if (entry.operation > result) {
            result = entry.operation;
        }
    }
    return result;
}
uint64_t FakeTransport::last_service_goal() const {
    uint64_t result = 0;
    for (const auto &entry : entries_) {
        if (entry.goal > result) {
            result = entry.goal;
        }
    }
    return result;
}
} // namespace ayni::runtime::multinode
