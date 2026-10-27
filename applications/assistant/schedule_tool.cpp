#include "policy_support.h"
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
constexpr char grammar_refusal[] =
    "rejected: expected 'remind once|daily|weekly YYYY-MM-DDTHH:MM <POSIX-TZ> <text of at most "
    "384 bytes>', 'check [<seconds, at least 300>]' or 'observe'; nothing was registered";
constexpr char capacity_refusal[] = "rejected: no room for another goal; nothing was registered";
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
    Schedule schedule;
    Encoded encoded;
    ayni_goal_create_v1 body{};
    std::string canonical;
};
// A model-origin top-level Goal owned by the calling instance; never a child of the chat Goal,
// which ends with its turn.
ayni_status_t build(const ayni_kernel_t *kernel, ayni_string_v1 input, uint64_t call,
                    uint64_t operation, GoalRequest &out) {
    auto status = parse_schedule(std::string(input.data, input.size), out.schedule);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    if (status == AYNI_OK) {
        status = ayni_get_call(kernel, call, &info);
    }
    if (status != AYNI_OK) {
        return status;
    }
    out.schedule.parameters.origin = Origin::model;
    out.schedule.parameters.operation_id = operation;
    status = prepare_goal(out.schedule, info.owner_instance_id, out.encoded, out.body);
    return status == AYNI_OK ? format_schedule(out.body, out.canonical) : status;
}
} // namespace
ayni_status_t find_operation(const ayni_kernel_t *kernel, uint64_t operation, uint64_t &out) {
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
        if (status == AYNI_OK && p.origin == Origin::model && p.operation_id == operation) {
            out = ids[at];
            return AYNI_OK;
        }
    }
    return status == AYNI_OK ? AYNI_NOT_FOUND : status;
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
// Decides one request. Everything that can fail or allocate runs before GoalPort::create, the
// only effect, so a refusal or failure honestly reports effect NONE. BUSY leaves it queued.
ayni_status_t ScheduleTool::answer(const ayni_kernel_t *kernel, GoalPort port, Slot &slot) {
    auto &receipt = slot.receipt;
    receipt = {};
    receipt.struct_size = sizeof(receipt);
    receipt.attempt = slot.attempt;
    receipt.outcome = AYNI_CALL_FAILED; // Until a decision below completes it.
    receipt.effect = AYNI_CALL_EFFECT_NONE;
    GoalRequest request;
    uint64_t goal = 0;
    bool registered = false;
    ayni_status_t status = AYNI_OK;
    try {
        status =
            build(kernel, {slot.input.data(), slot.input_size}, slot.call, slot.operation, request);
        if (status == AYNI_OK) {
            status = find_operation(kernel, slot.operation, goal);
            registered = status == AYNI_OK;
            status = status == AYNI_NOT_FOUND ? AYNI_OK : status;
        }
    } catch (const std::bad_alloc &) {
        receipt.status = AYNI_CAPACITY_EXCEEDED; // Memory, not Goal room: nothing was decided.
        return AYNI_OK;
    }
    if (status == AYNI_OK && !registered) {
        status = port.create ? port.create(port.context, &request.body, &goal) : AYNI_UNSUPPORTED;
    }
    if (status == AYNI_BUSY) {
        return AYNI_BUSY;
    }
    int written = 0;
    if (status == AYNI_OK) {
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        const bool proposed =
            ayni_get_goal(kernel, goal, &info) == AYNI_OK && info.lifecycle == AYNI_GOAL_PROPOSED;
        written =
            std::snprintf(slot.result.data(), slot.result.size(),
                          "registered goal %" PRIu64 ": %s; %s", goal, request.canonical.c_str(),
                          proposed ? "waiting for owner approval" : "already handled by the owner");
    } else if (status == AYNI_INVALID_ARGUMENT || status == AYNI_CAPACITY_EXCEEDED) {
        // The model can correct a request or tell the user; the turn continues.
        written =
            std::snprintf(slot.result.data(), slot.result.size(), "%s",
                          status == AYNI_INVALID_ARGUMENT ? grammar_refusal : capacity_refusal);
    } else {
        receipt.status = status; // No port, closed admission or a failed read: no Goal.
        return AYNI_OK;
    }
    // schedule_result_max covers the longest canonical schedule, so this cannot truncate.
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
