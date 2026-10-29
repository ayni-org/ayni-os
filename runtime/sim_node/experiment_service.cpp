#include "experiment_support.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <cstring>
namespace ayni::runtime::experiments {
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
ActionService::ActionService(ayni_call_query_authority_t query_authority) {
    provider_id_ = text("experiment.counter");
    capability_ = initialized<ayni_call_capability_v1>();
    capability_.version = 1;
    capability_.capability_id = text("test.increment");
    capability_.contract = {sizeof(ayni_call_contract_v1),
                            AYNI_CALL_ACTION,
                            AYNI_CALL_TOOL,
                            0,
                            AYNI_CALL_NOT_IDEMPOTENT,
                            query_authority,
                            AYNI_CALL_CANCEL_REQUEST,
                            0,
                            1000,
                            AYNI_CALL_INPUT_VALUE,
                            AYNI_CALL_RESULT_TEXT,
                            24,
                            0,
                            0,
                            nullptr,
                            nullptr};
    input_schema_.kind = result_schema_.kind = AYNI_VALUE_TEXT;
    input_schema_.subtree_end = result_schema_.subtree_end = 1;
    business_ = {{&input_schema_, 1}, {&result_schema_, 1}, 0, 0, nullptr};
    capability_.business = &business_;
}
ActionService::ActionService(const char *capability, const ayni_call_resource_v1 *resources,
                             uint32_t count, uint32_t effects)
    : ActionService() {
    provider_id_ = capability_.capability_id = text(capability);
    capability_.contract.effects = effects;
    input_schema_.max_count = effects == AYNI_CALL_EFFECT_NOTIFICATION ? 512U : 0U;
    capability_.contract.input_text_limit = input_schema_.max_count + 24;
    capability_.contract.resource_count = count;
    capability_.contract.resources = resources;
}
ayni_provider_v1 ActionService::describe() {
    auto out = initialized<ayni_provider_v1>();
    out.version = 1;
    out.provider_id = out.endpoint_id = provider_id_;
    out.ctx = this;
    out.capability_count = 1;
    out.capabilities = &capability_;
    out.submit = submit;
    out.cancel = cancel;
    if (capability_.contract.query != AYNI_CALL_QUERY_NONE) {
        out.query = query;
    }
    return out;
}
ayni_submit_result_t ActionService::submit(void *ctx, const ayni_provider_request_v1 *request,
                                           ayni_provider_completion_t *completion) {
    auto &self = *static_cast<ActionService *>(ctx);
    if (!request || !completion || request->struct_size != sizeof(*request) ||
        !request->operation_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ValueReader input;
    ayni_string_v1 body{};
    if (input.read(request->value) != AYNI_OK || !ValueReader::text(input.root(), body) ||
        body.size > self.input_schema_.max_count) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    for (auto &slot : self.calls_) {
        if (!slot.completion) {
            slot = {request->attempt, request->logical_call_id, request->operation_id, completion};
            ++self.submissions_;
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t ActionService::query(void *ctx, uint64_t operation,
                                          ayni_attempt_ref_v1 attempt,
                                          ayni_provider_completion_t *completion) {
    auto &self = *static_cast<ActionService *>(ctx);
    for (auto &slot : self.queries_) {
        if (!slot.completion && completion && operation) {
            slot = {attempt, 0, operation, completion};
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t ActionService::cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
    const auto &self = *static_cast<ActionService *>(ctx);
    for (const auto &slot : self.calls_) {
        if (slot.completion && slot.attempt.boot_epoch == attempt.boot_epoch &&
            slot.attempt.request_id == attempt.request_id) {
            // A request is not stop evidence; the scenario chooses when to post terminal.
            return AYNI_SUBMIT_ACCEPTED;
        }
    }
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
uint64_t ActionService::executions() const {
    uint64_t count = 0;
    for (const auto &effect : effects_) {
        count += effect.executions;
    }
    return count;
}
uint32_t ActionService::pending() const {
    return static_cast<uint32_t>(
        std::count_if(calls_.begin(), calls_.end(), [](const Borrow &v) { return v.completion; }));
}
uint32_t ActionService::pending_queries() const {
    return static_cast<uint32_t>(std::count_if(queries_.begin(), queries_.end(),
                                               [](const Borrow &v) { return v.completion; }));
}
ayni_status_t ActionService::execute(ayni_kernel_t *kernel) {
    for (auto &slot : calls_) {
        if (!slot.completion) {
            continue;
        }
        auto call = initialized<ayni_call_info_v1>();
        auto info = initialized<ayni_kernel_info_v1>();
        auto status = ayni_get_call(kernel, slot.call, &call);
        if (status == AYNI_OK) {
            status = ayni_get_kernel_info(kernel, &info);
        }
        if (status != AYNI_OK) {
            return status;
        }
        if (!call.requires_seq || call.requires_seq > info.durable_seq) {
            return AYNI_NOT_READY;
        }
        auto record = std::find_if(effects_.begin(), effects_.end(),
                                   [&](const Effect &v) { return v.operation == slot.operation; });
        if (record == effects_.end()) {
            record = std::find_if(effects_.begin(), effects_.end(),
                                  [](const Effect &v) { return !v.operation; });
        }
        if (record == effects_.end() || record->executions == UINT32_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        record->operation = slot.operation;
        ++record->executions;
        slot.executed = true;
        return AYNI_OK;
    }
    return AYNI_NOT_FOUND;
}
ayni_status_t ActionService::receipt(ayni_call_outcome_t outcome, ayni_call_effect_t effect) {
    for (auto &slot : calls_) {
        if (!slot.completion) {
            continue;
        }
        auto result = initialized<ayni_provider_result_v1>();
        result.outcome = outcome;
        result.effect = effect;
        result.attempt = slot.attempt;
        result.status = outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN
                        : outcome == AYNI_CALL_FAILED  ? AYNI_NOT_READY
                                                       : AYNI_OK;
        const auto status = ayni_provider_post(slot.completion, &result);
        if (status == AYNI_OK) {
            last_receipt_ = LastReceipt{slot.completion, result};
            slot = {};
        }
        return status;
    }
    return AYNI_NOT_FOUND;
}
ayni_status_t ActionService::answer_query(uint32_t conclusion) {
    for (auto &slot : queries_) {
        if (!slot.completion) {
            continue;
        }
        const bool executed = std::any_of(effects_.begin(), effects_.end(), [&](const Effect &v) {
            return v.operation == slot.operation && v.executions;
        });
        const bool in_flight = std::any_of(calls_.begin(), calls_.end(), [&](const Borrow &v) {
            return v.completion && v.operation == slot.operation;
        });
        if ((conclusion == AYNI_QUERY_RESULT_FOUND && !executed) ||
            (conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED && (executed || in_flight))) {
            return AYNI_INVALID_STATE;
        }
        auto result = initialized<ayni_provider_result_v1>();
        result.attempt = slot.attempt;
        result.query_result = conclusion;
        result.outcome = AYNI_CALL_ABANDONED;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
        result.status = AYNI_RESULT_UNKNOWN;
        if (conclusion == AYNI_QUERY_RESULT_FOUND) {
            result.outcome = AYNI_CALL_COMPLETED;
            result.effect = AYNI_CALL_EFFECT_COMPLETE;
            result.status = AYNI_OK;
        } else if (conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED) {
            // C-LIFE-010: affirmative nonexecution is FAILED/NONE, not an
            // ABANDONED result or permission to retry this operation automatically.
            result.outcome = AYNI_CALL_FAILED;
            result.effect = AYNI_CALL_EFFECT_NONE;
            result.status = AYNI_NOT_READY;
        }
        const auto status = ayni_provider_post(slot.completion, &result);
        if (status == AYNI_OK) {
            slot = {};
        }
        return status;
    }
    return AYNI_NOT_FOUND;
}
void ActionService::quiesce() {
    if (last_receipt_) {
        old_attempt_ = last_receipt_->result.attempt;
    }
    for (const auto &slot : calls_) {
        if (slot.completion) {
            old_attempt_ = slot.attempt;
            break;
        }
    }
    calls_ = {};
    queries_ = {};
    last_receipt_.reset();
}
ayni_status_t ActionService::repeat_receipt() {
    return last_receipt_ ? ayni_provider_post(last_receipt_->target, &last_receipt_->result)
                         : AYNI_NOT_FOUND;
}
ayni_status_t ActionService::post_old_attempt() {
    if (!old_attempt_) {
        return AYNI_NOT_FOUND;
    }
    for (const auto &slot : queries_) {
        if (slot.completion) {
            auto result = initialized<ayni_provider_result_v1>();
            result.attempt = *old_attempt_;
            result.outcome = AYNI_CALL_COMPLETED;
            result.effect = AYNI_CALL_EFFECT_COMPLETE;
            result.status = AYNI_OK;
            // Route an old packet to a currently valid target. Never dereference an
            // old Kernel pointer merely to test boot/attempt rejection (C-ID-006).
            return ayni_provider_post(slot.completion, &result);
        }
    }
    return AYNI_NOT_FOUND;
}
} // namespace ayni::runtime::experiments

namespace ayni::runtime::experiments {
ayni_status_t ActionService::drain() {
    while (pending()) {
        const auto &slot = *std::find_if(calls_.begin(), calls_.end(),
                                         [](const Borrow &v) { return v.completion; });
        const auto status = receipt(AYNI_CALL_CANCELLED, slot.executed ? AYNI_CALL_EFFECT_COMPLETE
                                                                       : AYNI_CALL_EFFECT_NONE);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
} // namespace ayni::runtime::experiments
