#include "motor_contract.h"
#include "motor_execution.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <type_traits>
namespace usbchat {
// Startup owns SDK resources until this succeeds; then ownership passes to the worker.
ayni_status_t MotorExecution::initialize() {
    auto rc = initialize_owner();
    if (rc != AYNI_OK) {
        return rc;
    }
    static_assert(std::is_trivially_copyable<Message>::value, "inbox messages own plain copies");
    const auto caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    inbox_tcb_ = static_cast<StaticQueue_t *>(heap_caps_malloc(sizeof(StaticQueue_t), caps));
    inbox_storage_ = static_cast<uint8_t *>(heap_caps_malloc(inbox_slots * sizeof(Message), caps));
    worker_tcb_ = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), caps));
    worker_stack_ = static_cast<StackType_t *>(heap_caps_malloc(worker_stack_bytes, caps));
    if (!inbox_tcb_ || !inbox_storage_ || !worker_tcb_ || !worker_stack_) {
        const bool cleaned = close_owner();
        release_worker_storage();
        return cleaned ? AYNI_CAPACITY_EXCEEDED : AYNI_INTERNAL;
    }
    inbox_ = xQueueCreateStatic(inbox_slots, sizeof(Message), inbox_storage_, inbox_tcb_);
    if (!inbox_) {
        const bool cleaned = close_owner();
        release_worker_storage();
        return cleaned ? AYNI_CAPACITY_EXCEEDED : AYNI_INTERNAL;
    }
    inbox_open_.store(true, std::memory_order_release);
    worker_ = xTaskCreateStatic(worker_entry, "motor-control", worker_stack_bytes, this,
                                worker_priority, worker_stack_, worker_tcb_);
    if (!worker_) {
        inbox_open_.store(false, std::memory_order_release);
        const bool cleaned = close_owner();
        release_worker_storage();
        return cleaned ? AYNI_CAPACITY_EXCEEDED : AYNI_INTERNAL;
    }
    return AYNI_OK;
}
bool MotorExecution::send(const Message &message) {
    return inbox_open_.load(std::memory_order_acquire) && inbox_ &&
           xQueueSend(inbox_, &message, 0) == pdTRUE;
}
ayni_submit_result_t MotorExecution::submit(const ayni_provider_request_v1 *request,
                                            ayni_provider_completion_t *completion) {
    if (!request || !completion || !request->execution_permit ||
        request->execution_permit->operation_id != request->operation_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    if (reader.read(request->value) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    int64_t values[3]{};
    for (uint32_t at = 0; at < 3; ++at) {
        const auto *field = reader.field(at + 1);
        if (!field || field->kind != AYNI_VALUE_I64) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        values[at] = field->value.integer;
    }
    if (values[0] < -motor_percent_max || values[0] > motor_percent_max ||
        values[1] < -motor_percent_max || values[1] > motor_percent_max ||
        values[2] < motor_duration_ms_min || values[2] > motor_plan_duration_ms_max) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    Message message{};
    message.kind = MessageKind::start;
    auto &start = message.start;
    start.command = {int32_t(values[0]), int32_t(values[1]), uint32_t(values[2])};
    if (!valid_motor_command(start.command) ||
        (!start.command.first_percent && !start.command.second_percent)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    start.attempt = request->attempt;
    start.completion = completion;
    start.permit = *request->execution_permit;
    start.identity = {0, request->logical_call_id, request->operation_id,
                      request->attempt.boot_epoch, request->attempt.request_id};
    return send(message) ? AYNI_SUBMIT_ACCEPTED : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t MotorExecution::cancel(ayni_attempt_ref_v1 attempt) {
    Message message{};
    message.kind = MessageKind::cancel;
    message.cancel = attempt;
    return send(message) ? AYNI_SUBMIT_ACCEPTED : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
void MotorExecution::receive(Kind kind, uint64_t id, const uint8_t *data, size_t size) {
    if (kind == Kind::motor_heartbeat && !size) {
        heartbeat_capture_.store(uint64_t(esp_timer_get_time()), std::memory_order_release);
        return;
    }
    if (size > motor_decision_bytes || (size && !data)) {
        request_stop();
        return;
    }
    Message message{};
    message.kind = MessageKind::frame;
    message.frame_kind = kind;
    message.frame_id = id;
    message.frame_size = size;
    if (size) {
        std::memcpy(message.frame.data(), data, size);
    }
    if (!send(message)) {
        request_stop();
    } // Control backpressure fails closed.
}
void MotorExecution::request_stop() {
    local_stop_.store(true, std::memory_order_release);
}
ayni_status_t MotorExecution::advance() {
    return worker_error_.load(std::memory_order_acquire);
}
void MotorExecution::publish_snapshot() {
    const uint32_t value =
        limit_ | (uint32_t(mode_) << snapshot_mode_shift) | (pending_ ? snapshot_busy : 0);
    snapshot_.store(value, std::memory_order_release);
}
void MotorExecution::drain_inbox() {
    Message message{};
    for (size_t at = 0; at < inbox_slots && xQueueReceive(inbox_, &message, 0) == pdTRUE; ++at) {
        switch (message.kind) {
        case MessageKind::start:
            begin(message.start);
            break;
        case MessageKind::cancel:
            if (pending_ && message.cancel.boot_epoch == pending_->attempt.boot_epoch &&
                message.cancel.request_id == pending_->attempt.request_id) {
                pending_->outcome = AYNI_CALL_CANCELLED;
                pending_->status = AYNI_DENIED;
            }
            break;
        case MessageKind::frame:
            receive_owner(message.frame_kind, message.frame_id, message.frame.data(),
                          message.frame_size);
            break;
        case MessageKind::port: {
            auto &slot = requests_[message.port.kind - 1];
            if (slot) {
                ayni_executor_response_v1 response{};
                response.struct_size = sizeof(response);
                response.kind = message.port.kind;
                response.request = message.port.identity;
                response.status = AYNI_BUSY;
                const auto rc = ayni_executor_post(message.port.completion, &response);
                if (rc != AYNI_OK && rc != AYNI_INVALID_STATE) {
                    worker_error_.store(rc);
                }
            } else {
                slot = message.port;
            }
            break;
        }
        }
    }
}
void MotorExecution::worker_entry(void *context) {
    static_cast<MotorExecution *>(context)->run();
}
// This task never calls Core queries/step and never waits for the operator, UART or storage.
void MotorExecution::run() {
    for (;;) {
        const auto now = uint64_t(esp_timer_get_time());
        if (last_cycle_us_) {
            cycle_gap_max_us_ = std::max(cycle_gap_max_us_, now - last_cycle_us_);
        }
        last_cycle_us_ = now;
        drain_inbox();
        const auto rc = advance_owner();
        if (rc != AYNI_OK && rc != AYNI_BUSY) {
            accepting_.store(false, std::memory_order_release);
            stop_owner();
            worker_error_.store(rc, std::memory_order_release);
        }
        publish_snapshot();
        if (closing_.load(std::memory_order_acquire) && close_owner()) {
            publish_snapshot();
            char metrics[128];
            const auto count = std::snprintf(
                metrics, sizeof(metrics),
                "metrics motor_control_stack_free=%lu/%lu gap_max_us=%llu",
                (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned long)worker_stack_bytes, (unsigned long long)cycle_gap_max_us_);
            if (count > 0 && size_t(count) < sizeof(metrics)) {
                transport_.send(Kind::status, 0, metrics, size_t(count));
            }
            worker_stopped_.store(true, std::memory_order_release);
            vTaskSuspend(nullptr); // Caller joins before freeing this stack/TCB.
        }
        vTaskDelay(1);
    }
}
void MotorExecution::release_worker_storage() {
    if (inbox_) {
        vQueueDelete(inbox_);
        inbox_ = nullptr;
    }
    heap_caps_free(inbox_storage_);
    inbox_storage_ = nullptr;
    heap_caps_free(inbox_tcb_);
    inbox_tcb_ = nullptr;
    heap_caps_free(worker_stack_);
    worker_stack_ = nullptr;
    heap_caps_free(worker_tcb_);
    worker_tcb_ = nullptr;
}
bool MotorExecution::close() {
    inbox_open_.store(false, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    if (!worker_) {
        return true;
    }
    closing_.store(true, std::memory_order_release);
    if (!worker_stopped_.load(std::memory_order_acquire) || eTaskGetState(worker_) != eSuspended) {
        return false;
    }
    // IDF6 legacy dual-CPU join: suspension excludes execution before freeing static memory.
    vTaskDelete(worker_);
    worker_ = nullptr;
    release_worker_storage();
    return true;
}
} // namespace usbchat
