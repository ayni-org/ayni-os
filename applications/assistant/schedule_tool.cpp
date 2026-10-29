#include "policy_support.h"
#include <algorithm>
#include <ayni/applications/assistant/schedule_tool.h>
#include <cinttypes>
#include <cstdio>
#include <new>
namespace ayni::applications::assistant {
namespace {
using detail::text;
// Finite request duration; settle answers on the next owner pass unless the port defers.
constexpr uint32_t schedule_timeout_ms = 60000;
constexpr char endpoint[] = "assistant.local";
// Model-facing texts state the admission limits in words; keep them equal to the constants.
static_assert(text_bytes_max == 384 && check_period_us == 300000000ULL,
              "update the grammar refusal and usage descriptor");
constexpr char grammar_refusal[] =
    "rejected: expected 'remind once|daily|weekly YYYY-MM-DDTHH:MM <POSIX-TZ> <text of at most "
    "384 bytes>', 'check [<seconds, at least 300>]' or 'observe'; nothing was registered";
constexpr char capacity_refusal[] = "rejected: no room for another goal; nothing was registered";
constexpr char waiting_refusal[] =
    "rejected: earlier goals still wait for owner approval; ask the owner to approve or cancel "
    "them first; nothing was registered";
constexpr char registered_format[] = "registered goal %" PRIu64 ": %s; %s";
constexpr char waiting_state[] = "waiting for owner approval";
constexpr char handled_state[] = "already handled by the owner";
constexpr size_t goal_id_digits_max = 20; // UINT64_MAX in decimal.
// The format's own conversion specifiers are replaced, so this bound is generous.
static_assert(schedule_result_max > sizeof(registered_format) + goal_id_digits_max +
                                        schedule_canonical_max +
                                        std::max(sizeof(waiting_state), sizeof(handled_state)),
              "a registration answer must never truncate");
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
ayni_status_t build(ayni_string_v1 input, uint64_t owner_instance, uint64_t operation,
                    GoalRequest &out) {
    auto status = parse_schedule(std::string(input.data, input.size), out.schedule);
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
                            AYNI_CALL_INPUT_FIELDS_TEXT,
                            AYNI_CALL_RESULT_TEXT,
                            schedule_text_max,
                            schedule_result_max,
                            0,
                            nullptr,
                            nullptr};
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
    return {sizeof(ayni_tool_usage_v1),
            binding.binding_version,
            binding.binding_id,
            capability_.version,
            capability_.capability_id,
            text("Register one lasting reminder, device check or scene observation requested by "
                 "the owner. Resolve relative dates from the supplied current local time and "
                 "use its timezone unless the user names another; ask if unknown."),
            text("Exactly one command: 'remind once|daily|weekly YYYY-MM-DDTHH:MM <POSIX-TZ> "
                 "<text of at most 384 bytes>', 'check [<seconds, at least 300>]' or 'observe'. "
                 "Timezone is a POSIX rule such as CST-8 or UTC0. Empty text is invalid."),
            text("'registered goal N: <schedule>; waiting for owner approval' registers a "
                 "proposal; it does not run or approve the work. 'rejected: ...; nothing was "
                 "registered' asks for corrected input or capacity/approval resolution."),
            text("remind once 2026-10-30T09:00 CST-8 喝水; check 300; observe"),
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
        !same(r->endpoint_id, text(endpoint)) || r->arguments.field_count ||
        r->text.size > schedule_text_max || (r->text.size && !r->text.data)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    for (auto &slot : self.slots_) {
        if (slot.phase != Phase::empty) {
            continue;
        }
        slot.phase = Phase::queued;
        slot.attempt = r->attempt;
        slot.call = r->logical_call_id;
        slot.operation = r->operation_id;
        slot.completion = completion;
        slot.input_size = r->text.size;
        if (r->text.size) {
            std::memcpy(slot.input.data(), r->text.data, r->text.size);
        }
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
        status = build({slot.input.data(), slot.input_size}, call.owner_instance_id, slot.operation,
                       request);
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
        refusal = status == AYNI_INVALID_ARGUMENT ? grammar_refusal : capacity_refusal;
    } else if (status != AYNI_OK) {
        receipt.status = status; // No port, closed admission or a failed read: no Goal.
        return AYNI_OK;
    }
    int written = 0;
    if (refusal) {
        // The model can correct a request or tell the user; the turn continues.
        written = std::snprintf(slot.result.data(), slot.result.size(), "%s", refusal);
    } else {
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        const bool waiting =
            ayni_get_goal(kernel, goal, &info) == AYNI_OK && info.lifecycle == AYNI_GOAL_PROPOSED;
        written = std::snprintf(slot.result.data(), slot.result.size(), registered_format, goal,
                                request.canonical.c_str(), waiting ? waiting_state : handled_state);
    }
    if (written <= 0 || static_cast<size_t>(written) >= slot.result.size()) {
        return AYNI_INTERNAL;
    }
    slot.result_size = static_cast<uint32_t>(written);
    receipt.outcome = AYNI_CALL_COMPLETED;
    receipt.status = AYNI_OK;
    receipt.effect = AYNI_CALL_EFFECT_COMPLETE;
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
        slot.receipt.result = {slot.result.data(), slot.result_size};
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
