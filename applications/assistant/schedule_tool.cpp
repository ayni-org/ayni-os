#include "policy_support.h"
#include "schedule_value.h"
#include <algorithm>
#include <ayni/applications/assistant/schedule_tool.h>
#include <ayni/values.hpp>
#include <cinttypes>
#include <cstdio>
#include <new>
namespace ayni::applications::assistant {
namespace {
using detail::text;
// Finite request duration; settle answers on the next owner pass unless the port defers.
constexpr uint32_t schedule_timeout_ms = 60000;
constexpr char endpoint[] = "assistant.local";
// Usage/refusal prose names these product bounds; prevent drift from the contract.
static_assert(text_bytes_max == 384 && detail::minimum_period_seconds == 300,
              "update structured schedule usage/refusal bounds");
enum ResultState : uint32_t { waiting_owner, handled, no_goal };
constexpr char input_refusal[] =
    "rejected: expected structured schedule fields; remind needs local_time YYYY-MM-DDTHH:MM, "
    "a POSIX timezone and nonempty text; check needs period_seconds >= 300; "
    "irrelevant fields must retain their defaults; nothing was registered";
constexpr char capacity_refusal[] = "rejected: no room for another goal; nothing was registered";
constexpr char waiting_refusal[] =
    "rejected: earlier goals still wait for owner approval; ask the owner to approve or cancel "
    "them first; nothing was registered";
constexpr uint32_t result_status = 1, result_goal = 2, result_state = 3, result_canonical = 4,
                   result_message = 5;
constexpr uint32_t message_bytes_max = sizeof(input_refusal) - 1;
const ayni_string_v1 kinds[] = {text("remind"), text("check"), text("observe")};
const ayni_string_v1 frequencies[] = {text("once"), text("daily"), text("weekly")};
const ayni_string_v1 statuses[] = {text("registered"), text("rejected")};
const ayni_string_v1 states[] = {text("waiting_owner"), text("handled"), text("none")};
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && a.data && b.data && std::memcmp(a.data, b.data, a.size) == 0;
}
bool schedule_type(const ayni_goal_info_v1 &goal) {
    for (const auto kind : {GoalKind::reminder, GoalKind::device_check, GoalKind::scene_observe}) {
        if (same({goal.goal_type_id, goal.goal_type_id_size}, text(goal_type(kind)))) {
            return true;
        }
    }
    return false;
}
// The exact GoalCreate one operation asks for and its canonical text for owner review. body
// borrows schedule and encoded, so a GoalRequest is filled in place and never copied.
struct GoalRequest {
    GoalRequest() = default;
    GoalRequest(const GoalRequest &) = delete;
    GoalRequest &operator=(const GoalRequest &) = delete;
    Schedule schedule;
    Encoded encoded;
    ayni_goal_create_v1 body{};
    std::string canonical;
};
// A model-origin top-level Goal owned by the calling instance; never a child of the chat Goal,
// which ends with its turn.
ayni_status_t build(ayni_value_v1 input, uint64_t owner_instance, uint64_t operation,
                    GoalRequest &out) {
    auto status = detail::read_schedule(input, out.schedule);
    if (status != AYNI_OK) {
        return status;
    }
    out.schedule.parameters.origin = Origin::model;
    out.schedule.parameters.operation_id = operation;
    status = prepare_goal(out.schedule, owner_instance, out.encoded, out.body);
    return status == AYNI_OK ? format_schedule(out.body, out.canonical) : status;
}
// The Task that asked has ended or is ending (stop, cancel, deadline): its request is moot.
bool caller_ended(const ayni_call_info_v1 &call, const ayni_task_info_v1 &task) {
    return call.cancel_requested || task.lifecycle == AYNI_TASK_CANCELLING ||
           task.lifecycle == AYNI_TASK_CANCELLED || task.lifecycle == AYNI_TASK_COMPLETED ||
           task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_EXPIRED;
}
} // namespace
ayni_status_t model_goals(const ayni_kernel_t *kernel, uint64_t operation, ModelGoals &out) {
    ModelGoals found;
    std::vector<uint64_t> ids;
    auto status = detail::objects(kernel, AYNI_OBJECT_GOAL, ids);
    for (size_t at = 0; status == AYNI_OK && at < ids.size(); ++at) {
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        status = ayni_get_goal(kernel, ids[at], &goal);
        if (status != AYNI_OK || !schedule_type(goal)) {
            continue;
        }
        Encoded encoded;
        status = ayni_get_goal_parameters(kernel, ids[at], encoded.bytes.data(),
                                          parameter_bytes_max, &encoded.size);
        Parameters p;
        if (status == AYNI_OK) {
            status = decode(encoded.view(), p);
        }
        if (status != AYNI_OK || p.origin != Origin::model) {
            continue;
        }
        if (operation && p.operation_id == operation) {
            found.registered = ids[at];
        }
        if (goal.lifecycle == AYNI_GOAL_PROPOSED) {
            found.waiting.push_back(ids[at]);
        }
    }
    if (status == AYNI_OK) {
        out = std::move(found);
    }
    return status;
}
ScheduleTool::ScheduleTool() {
    capability_.struct_size = sizeof(capability_);
    capability_.version = 1;
    capability_.capability_id = text("assistant.schedule");
    // Registering a Goal is local bookkeeping: no egress, charge, notification or actuator.
    capability_.contract = {sizeof(ayni_call_contract_v1),
                            AYNI_CALL_ACTION,
                            AYNI_CALL_TOOL,
                            0,
                            AYNI_CALL_NOT_IDEMPOTENT,
                            AYNI_CALL_QUERY_NONE,
                            AYNI_CALL_CANCEL_NONE,
                            0,
                            schedule_timeout_ms,
                            AYNI_CALL_INPUT_VALUE,
                            AYNI_CALL_RESULT_VALUE,
                            schedule_text_max,
                            schedule_result_max,
                            0,
                            nullptr,
                            nullptr};
    build_input_schema();
    build_result_schema();
    business_ = {{input_schema_.data(), static_cast<uint32_t>(input_schema_.size())},
                 {result_schema_.data(), static_cast<uint32_t>(result_schema_.size())},
                 0,
                 0,
                 nullptr};
    capability_.business = &business_;
}
void ScheduleTool::build_input_schema() {
    // Optional operands are filled exactly once by Core. The application then rejects
    // combinations irrelevant to kind instead of discarding supplied information.
    input_schema_[0].kind = AYNI_VALUE_OBJECT;
    input_schema_[0].subtree_end = static_cast<uint32_t>(input_schema_.size());
    const char *names[] = {"kind", "frequency", "local_time", "timezone", "text", "period_seconds"};
    for (uint32_t at = 1; at < input_schema_.size(); ++at) {
        auto &node = input_schema_[at];
        node.field_id = at;
        node.subtree_end = at + 1U;
        node.name = text(names[at - 1U]);
        node.kind = at <= detail::frequency ? AYNI_VALUE_ENUM : AYNI_VALUE_TEXT;
        if (at == detail::kind) {
            node.flags = AYNI_SCHEMA_REQUIRED;
        } else {
            const uint32_t index = at - 2U;
            auto &value = defaults_[index];
            value.kind = node.kind;
            value.subtree_end = 1;
            value.value.text = text(at == detail::frequency ? "once" : "");
            default_views_[index] = {&value, 1};
            node.default_value = &default_views_[index];
        }
    }
    input_schema_[detail::kind].choices = kinds;
    input_schema_[detail::kind].choice_count = std::size(kinds);
    input_schema_[detail::frequency].choices = frequencies;
    input_schema_[detail::frequency].choice_count = std::size(frequencies);
    input_schema_[detail::local_time].max_count = sizeof("YYYY-MM-DDTHH:MM") - 1U;
    input_schema_[detail::timezone].max_count = AYNI_CALENDAR_TZ_MAX_BYTES;
    input_schema_[detail::body].max_count = text_bytes_max;
    auto &period = input_schema_[detail::period_seconds];
    period.kind = AYNI_VALUE_U64;
    period.unit = text("second");
    period.min_u64 = detail::minimum_period_seconds;
    period.max_u64 = UINT64_MAX / detail::seconds_to_us;
    defaults_.back().kind = AYNI_VALUE_U64;
    defaults_.back().value.unsigned_integer = detail::minimum_period_seconds;
}
void ScheduleTool::build_result_schema() {
    result_schema_[0].kind = AYNI_VALUE_OBJECT;
    result_schema_[0].subtree_end = static_cast<uint32_t>(result_schema_.size());
    const char *result_names[] = {"status", "goal_id", "state", "canonical", "message"};
    for (uint32_t at = 1; at < result_schema_.size(); ++at) {
        auto &node = result_schema_[at];
        node.field_id = at;
        node.subtree_end = at + 1U;
        node.flags = AYNI_SCHEMA_REQUIRED;
        node.name = text(result_names[at - 1U]);
        node.kind = AYNI_VALUE_TEXT;
    }
    result_schema_[result_status].kind = AYNI_VALUE_ENUM;
    result_schema_[result_status].choices = statuses;
    result_schema_[result_status].choice_count = std::size(statuses);
    result_schema_[result_goal].kind = AYNI_VALUE_U64;
    result_schema_[result_goal].flags |= AYNI_SCHEMA_NULLABLE;
    result_schema_[result_goal].min_u64 = 1;
    result_schema_[result_goal].max_u64 = UINT64_MAX;
    result_schema_[result_state].kind = AYNI_VALUE_ENUM;
    result_schema_[result_state].choices = states;
    result_schema_[result_state].choice_count = std::size(states);
    result_schema_[result_canonical].max_count = schedule_canonical_max;
    result_schema_[result_message].max_count = message_bytes_max;
}
ayni_provider_v1 ScheduleTool::describe() {
    return {sizeof(ayni_provider_v1),
            1,
            text("assistant.schedule"),
            text(endpoint),
            {},
            this,
            1,
            0,
            &capability_,
            submit,
            nullptr,
            nullptr,
            nullptr};
}
ayni_tool_usage_v1 ScheduleTool::usage(ayni_tool_ref_v1 binding) const {
    return {
        sizeof(ayni_tool_usage_v1),
        binding.binding_version,
        binding.binding_id,
        capability_.version,
        capability_.capability_id,
        text("Register one lasting reminder, device check or scene observation requested by "
             "the owner. Resolve relative dates from the supplied current local time and "
             "use its timezone unless the user names another; ask if unknown."),
        text("An object: kind=remind|check|observe. Remind needs local_time in "
             "YYYY-MM-DDTHH:MM, timezone as a POSIX rule such as CST-8 or UTC0, "
             "nonempty text of at most 384 bytes, frequency=once|daily|weekly. "
             "Check uses period_seconds >= 300. Other fields keep their Schema defaults."),
        text("status=registered gives goal_id, canonical schedule and state=waiting_owner or "
             "handled. It does not run or approve the work. status=rejected gives no "
             "Goal and explains the input, capacity or approval problem in message."),
        text(
            R"({"kind":"remind","local_time":"2026-10-30T09:00","timezone":"CST-8","text":"喝水"})"),
        0,
        0,
        nullptr};
}
ayni_submit_result_t ScheduleTool::submit(void *context, const ayni_provider_request_v1 *r,
                                          ayni_provider_completion_t *completion) {
    auto &self = *static_cast<ScheduleTool *>(context);
    if (!r || !completion || r->struct_size != sizeof(*r) || !r->attempt.boot_epoch ||
        !r->attempt.request_id || !r->logical_call_id || !r->operation_id ||
        !same(r->capability_id, self.capability_.capability_id) || r->capability_version != 1 ||
        !same(r->endpoint_id, text(endpoint))) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (ayni_value_validate(&r->value) != AYNI_OK || r->value.nodes[0].kind != AYNI_VALUE_OBJECT) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    for (auto &slot : self.slots_) {
        if (slot.phase != Phase::empty) {
            continue;
        }
        if (ayni_value_write(&r->value, slot.input.data(), schedule_text_max, &slot.input_size) !=
            AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        slot.phase = Phase::queued;
        slot.attempt = r->attempt;
        slot.call = r->logical_call_id;
        slot.operation = r->operation_id;
        slot.completion = completion;
        return AYNI_SUBMIT_ACCEPTED;
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
// Decides one request. Every read, check and allocation runs before GoalPort::create, the only
// effect, so a failure or a moot request honestly reports effect NONE. A refusal is a COMPLETED
// answer whose text says nothing was registered. BUSY leaves the request queued.
ayni_status_t ScheduleTool::answer(const ayni_kernel_t *kernel, GoalPort port, Slot &slot) {
    auto &receipt = slot.receipt;
    receipt = {};
    receipt.struct_size = sizeof(receipt);
    receipt.attempt = slot.attempt;
    receipt.outcome = AYNI_CALL_FAILED; // Until a decision below completes it.
    receipt.effect = AYNI_CALL_EFFECT_NONE;
    ayni_call_info_v1 call{};
    call.struct_size = sizeof(call);
    ayni_task_info_v1 task{};
    task.struct_size = sizeof(task);
    auto status = ayni_get_call(kernel, slot.call, &call);
    if (status == AYNI_OK) {
        status = ayni_get_task(kernel, call.task_id, &task);
    }
    if (status != AYNI_OK) {
        receipt.status = status;
        return AYNI_OK;
    }
    if (caller_ended(call, task)) {
        receipt.outcome = AYNI_CALL_CANCELLED; // "停" may land between submit and settle.
        receipt.status = AYNI_OK;
        return AYNI_OK;
    }
    GoalRequest request;
    ModelGoals goals;
    try {
        std::array<ayni_value_node_v1, schedule_input_nodes> nodes{};
        uint32_t count = 0;
        status = ayni_value_read({slot.input.data(), slot.input_size}, nodes.data(), nodes.size(),
                                 &count);
        if (status == AYNI_OK) {
            status = build({nodes.data(), count}, call.owner_instance_id, slot.operation, request);
        }
        if (status == AYNI_OK && request.canonical.size() >= slot.canonical.size()) {
            status = AYNI_INTERNAL; // Refuse before GoalPort: a result must always fit.
        }
        if (status == AYNI_OK) {
            status = model_goals(kernel, slot.operation, goals);
        }
    } catch (const std::bad_alloc &) {
        receipt.status = AYNI_CAPACITY_EXCEEDED; // Memory, not Goal room: nothing was decided.
        return AYNI_OK;
    }
    const char *refusal = nullptr;
    uint64_t goal = goals.registered;
    if (status == AYNI_OK && !goal) {
        if (goals.waiting.size() >= model_goals_waiting_max) {
            refusal = waiting_refusal;
        } else {
            status =
                port.create ? port.create(port.context, &request.body, &goal) : AYNI_UNSUPPORTED;
        }
    }
    if (status == AYNI_BUSY) {
        return AYNI_BUSY;
    }
    if (status == AYNI_INVALID_ARGUMENT || status == AYNI_CAPACITY_EXCEEDED) {
        refusal = status == AYNI_INVALID_ARGUMENT ? input_refusal : capacity_refusal;
    } else if (status != AYNI_OK) {
        receipt.status = status; // No port, closed admission or a failed read: no Goal.
        return AYNI_OK;
    }
    return complete(kernel, slot, goal, request.canonical, refusal);
}
// Builds a fixed receipt without allocation after Goal admission. Its borrowed text
// lives in the slot until the terminal post returns the completion borrow.
ayni_status_t ScheduleTool::complete(const ayni_kernel_t *kernel, Slot &slot, uint64_t goal,
                                     const std::string &canonical, const char *refusal) {
    auto &result = slot.result;
    result.clear();
    result.object();
    result.enumeration(result_status, statuses[refusal ? 1U : 0U]);
    if (refusal) {
        result.null(result_goal);
        result.enumeration(result_state, states[no_goal]);
        result.text(result_canonical, text(""));
        result.text(result_message, text(refusal));
    } else {
        std::memcpy(slot.canonical.data(), canonical.data(), canonical.size());
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        const auto read = ayni_get_goal(kernel, goal, &info);
        if (read != AYNI_OK) {
            return read; // Keep the queued operation; never invent an approval state.
        }
        const bool waiting = info.lifecycle == AYNI_GOAL_PROPOSED;
        result.unsigned_integer(result_goal, goal);
        result.enumeration(result_state, states[waiting ? waiting_owner : handled]);
        result.text(result_canonical,
                    {slot.canonical.data(), static_cast<uint32_t>(canonical.size())});
        result.text(result_message, text(""));
    }
    result.end();
    if (result.finish(slot.receipt.value) != AYNI_OK) {
        return AYNI_INTERNAL;
    }
    slot.receipt.outcome = AYNI_CALL_COMPLETED;
    slot.receipt.status = AYNI_OK;
    slot.receipt.effect = AYNI_CALL_EFFECT_COMPLETE;
    return AYNI_OK;
}
ayni_status_t ScheduleTool::settle(const ayni_kernel_t *kernel, GoalPort port) {
    for (auto &slot : slots_) {
        if (slot.phase == Phase::queued) {
            const auto status = answer(kernel, port, slot);
            if (status == AYNI_BUSY) {
                continue;
            }
            if (status != AYNI_OK) {
                return status;
            }
            slot.phase = Phase::answered;
        }
        if (slot.phase != Phase::answered) {
            continue;
        }
        const auto posted = ayni_provider_post(slot.completion, &slot.receipt);
        if (posted == AYNI_BUSY) {
            continue; // Keep the fixed receipt; the Goal decision is never repeated.
        }
        if (posted != AYNI_OK) {
            return posted;
        }
        slot = {};
    }
    return AYNI_OK;
}
uint32_t ScheduleTool::pending() const {
    uint32_t count = 0;
    for (const auto &slot : slots_) {
        count += slot.phase == Phase::empty ? 0U : 1U;
    }
    return count;
}
} // namespace ayni::applications::assistant
