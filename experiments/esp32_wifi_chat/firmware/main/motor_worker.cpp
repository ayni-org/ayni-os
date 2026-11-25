#include "motor_contract.h"
#include "motor_execution.h"
#include "wake.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <type_traits>
namespace wifichat {
ayni_status_t MotorExecution::reserve_control_stack() {
    if (initialization_ != Initialization::unattempted || mode_ == Mode::closed ||
        closing_.load(std::memory_order_acquire)) {
        return AYNI_NOT_READY;
    }
    const auto caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (!worker_stack_) {
        worker_stack_ = static_cast<StackType_t *>(heap_caps_malloc(worker_stack_bytes, caps));
    }
    const auto code = worker_stack_ ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
    transport_.diagnostics().record("motor init stage=stack-reserve status=%ld internal_free=%zu "
                                    "internal_largest=%zu stack_bytes=%lu\n",
                                    long(code), heap_caps_get_free_size(caps),
                                    heap_caps_get_largest_free_block(caps),
                                    static_cast<unsigned long>(worker_stack_bytes));
    return code;
}
// Startup owns SDK resources until this succeeds; then ownership passes to the worker.
ayni_status_t MotorExecution::initialize() {
    if (initialization_ == Initialization::ready && worker_) {
        return AYNI_OK;
    }
    if (initialization_ != Initialization::unattempted || mode_ == Mode::closed ||
        closing_.load(std::memory_order_acquire)) {
        return AYNI_NOT_READY;
    }
    initialization_ = Initialization::failed;
    const auto caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const auto trace_init = [&](const char *stage, ayni_status_t code, esp_err_t sdk) {
        transport_.diagnostics().record(
            "motor init stage=%s status=%ld sdk=%ld internal_free=%zu internal_largest=%zu "
            "inbox_bytes=%zu stack_bytes=%lu gate_bytes=%zu\n",
            stage, long(code), long(sdk), heap_caps_get_free_size(caps),
            heap_caps_get_largest_free_block(caps), inbox_slots * sizeof(Message),
            static_cast<unsigned long>(worker_stack_bytes), gate_bytes_);
    };
    const auto abort_init = [&](const char *stage, ayni_status_t code) {
        trace_init(stage, code, output_ ? output_->last_error() : ESP_OK);
        const bool cleaned = close_owner();
        release_worker_storage();
        return cleaned ? code : AYNI_INTERNAL;
    };
    trace_init("begin", AYNI_OK, ESP_OK);
    // The owner reserved this contiguous block before Host startup; preparation stays
    // hardware-only.
    if (!worker_stack_) {
        return abort_init("stack", AYNI_CAPACITY_EXCEEDED);
    }
    auto rc = initialize_owner();
    if (rc != AYNI_OK) {
        return abort_init("driver-or-gate", rc);
    }
    static_assert(std::is_trivially_copyable<Message>::value, "inbox messages own plain copies");
    inbox_tcb_ = static_cast<StaticQueue_t *>(heap_caps_malloc(sizeof(StaticQueue_t), caps));
    // Only owner/control tasks copy these messages; no ISR touches this bulk storage.
    // Queue control, worker stack, Gate and RMT source buffers remain in internal RAM.
    inbox_storage_ = static_cast<uint8_t *>(
        heap_caps_malloc(inbox_slots * sizeof(Message), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    worker_tcb_ = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), caps));
    if (!inbox_tcb_ || !inbox_storage_ || !worker_tcb_ || !worker_stack_) {
        return abort_init("inbox-allocation", AYNI_CAPACITY_EXCEEDED);
    }
    inbox_ = xQueueCreateStatic(inbox_slots, sizeof(Message), inbox_storage_, inbox_tcb_);
    if (!inbox_) {
        return abort_init("inbox-create", AYNI_CAPACITY_EXCEEDED);
    }
    worker_ = xTaskCreateStatic(worker_entry, "motor-control", worker_stack_bytes, this,
                                worker_priority, worker_stack_, worker_tcb_);
    if (!worker_) {
        return abort_init("worker-create", AYNI_CAPACITY_EXCEEDED);
    }
    inbox_access_.fetch_and(~inbox_closed, std::memory_order_release);
    initialization_ = Initialization::ready;
    // Driver ownership has passed to the worker; startup must no longer inspect it.
    trace_init("ready", AYNI_OK, ESP_OK);
    return AYNI_OK;
}
bool MotorExecution::send(const Message &message) {
    // The producer holds a borrow across the FreeRTOS queue call. Closing does
    // not free the backing memory until producers and the worker have left it.
    const auto before = inbox_access_.fetch_add(1U, std::memory_order_acquire);
    const bool sent = (before & inbox_closed) == 0U && xQueueSend(inbox_, &message, 0) == pdTRUE;
    inbox_access_.fetch_sub(1U, std::memory_order_release);
    return sent;
}
ayni_submit_result_t MotorExecution::submit(const ayni_provider_request_v1 *request,
                                            ayni_provider_completion_t *completion) {
    if (!request || !completion || !request->execution_permit ||
        request->execution_permit->operation_id != request->operation_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto &tree = request->value;
    if (!tree.nodes || tree.node_count < 6 || tree.nodes[0].kind != AYNI_VALUE_OBJECT ||
        tree.nodes[0].subtree_end != tree.node_count) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    uint32_t list_idx = 0;
    for (uint32_t at = 1; at < tree.nodes[0].subtree_end; at = tree.nodes[at].subtree_end) {
        if (tree.nodes[at].field_id == 1 && tree.nodes[at].kind == AYNI_VALUE_LIST) {
            list_idx = at;
            break;
        }
    }
    if (!list_idx) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    Message message{};
    message.kind = MessageKind::start;
    auto &start = message.start;
    for (uint32_t item = list_idx + 1; item < tree.nodes[list_idx].subtree_end;
         item = tree.nodes[item].subtree_end) {
        if (start.trajectory.count >= motor_segments_max ||
            tree.nodes[item].kind != AYNI_VALUE_OBJECT) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        int64_t values[3]{};
        uint32_t seen = 0;
        for (uint32_t f = item + 1; f < tree.nodes[item].subtree_end;
             f = tree.nodes[f].subtree_end) {
            const auto &node = tree.nodes[f];
            if (node.field_id < 1 || node.field_id > 3 || node.kind != AYNI_VALUE_I64) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            values[node.field_id - 1] = node.value.integer;
            seen |= 1u << (node.field_id - 1);
        }
        if (seen != 0x7u || values[0] < -int64_t(motor_percent_max) ||
            values[0] > motor_percent_max || values[1] < -int64_t(motor_percent_max) ||
            values[1] > motor_percent_max || values[2] < motor_duration_ms_min ||
            values[2] > motor_plan_duration_ms_max) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        start.trajectory.segments[start.trajectory.count++] = {
            int32_t(values[0]), int32_t(values[1]), uint32_t(values[2])};
    }
    if (!valid_motor_trajectory(start.trajectory)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    start.attempt = request->attempt;
    start.call = request->logical_call_id;
    start.completion = completion;
    start.permit = *request->execution_permit;
    const auto dispatched = uint64_t(esp_timer_get_time());
    start.target_seq = transport_.control_mailbox().reserve_start();
    if (!start.target_seq || !send(message)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    // Core calls this Provider only after its dispatch barrier is already durable.
    latest_dispatch_ = start.call;
    trace_handoff("dispatch", start.call, dispatched);
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t MotorExecution::submit_stream(ayni_attempt_ref_v1 attempt, uint64_t call,
                                                   ayni_provider_completion_t *completion,
                                                   const ayni_execution_permit_v1 &permit,
                                                   const MotorCommand &initial,
                                                   uint64_t session_goal, uint64_t expires_at_us,
                                                   uint64_t start_seq) {
    if (!completion || !session_goal || !valid_motor_command(initial) ||
        permit.form != AYNI_FORM_SESSION) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    Message message{};
    message.kind = MessageKind::start;
    auto &start = message.start;
    start.attempt = attempt;
    start.call = call;
    start.completion = completion;
    start.permit = permit;
    start.trajectory.count = 1;
    start.trajectory.segments[0] = initial;
    start.streaming = true;
    start.deadline_us = expires_at_us;
    start.session_goal = session_goal;
    start.target_seq = start_seq;
    if (!start.target_seq || transport_.control_mailbox().target_blocked(start_seq) ||
        !send(message)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    latest_dispatch_ = call;
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t MotorExecution::submit_soft_stop(const ayni_provider_request_v1 *request,
                                                      ayni_provider_completion_t *completion) {
    if (!request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    transport_.request_motor_soft_stop();
    Message message{};
    message.kind = MessageKind::soft_stop;
    message.soft_stop = {request->attempt, completion};
    if (!send(message)) {
        // Still engage the overflow-proof soft-stop latch so PWM stops even under inbox pressure.
        transport_.request_motor_soft_stop();
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return AYNI_SUBMIT_ACCEPTED;
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
    if (size > sizeof(uint32_t) || (size && !data)) {
        request_stop();
        return;
    }
    Message message{};
    message.kind = MessageKind::frame;
    message.frame_kind = kind;
    message.frame_id = id;
    message.frame_size = uint32_t(size);
    if (size) {
        std::memcpy(message.frame.data(), data, size);
    }
    if (!send(message)) {
        request_stop();
    } // Control backpressure fails closed.
}
void MotorExecution::request_stop() {
    transport_.request_motor_stop();
}
void MotorExecution::request_soft_stop() {
    transport_.request_motor_soft_stop();
    wake_control();
}
void MotorExecution::wake_control() {
    // A full queue already wakes the worker; the atomic interlock remains set.
    Message message{};
    message.kind = MessageKind::wake;
    static_cast<void>(send(message));
}
ayni_status_t MotorExecution::advance() {
    if (initialization_ == Initialization::failed && !worker_ && !initialization_reported_) {
        constexpr char failure[] = "motor initialization failed";
        initialization_reported_ = transport_.send(Kind::status, 0, failure, sizeof(failure) - 1);
    }
    return worker_error_.load(std::memory_order_acquire);
}
void MotorExecution::publish_snapshot() {
    const uint32_t value =
        limit_ | (uint32_t(mode_) << snapshot_mode_shift) | (pending_ ? snapshot_busy : 0);
    state_notice_ = state_notice_ || value != snapshot_.load(std::memory_order_relaxed);
    snapshot_.store(value, std::memory_order_release);
    ControlSessionTarget desired;
    if (pending_ && pending_->streaming && pending_->phase != Phase::result) {
        desired = {pending_->session_goal, pending_->target_seq,
                   int32_t(session_segments_[0].fields[0].value),
                   int32_t(session_segments_[0].fields[1].value)};
    }
    transport_.control_mailbox().publish_mode(
        uint8_t(mode_), pending_ && pending_->phase == Phase::running,
        pending_ && pending_->streaming && hold_scale_ < AYNI_CONTROL_LOOP_FULL_SCALE, desired);
}
void MotorExecution::handle_message(const Message &message) {
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
    case MessageKind::wake:
        break; // The caller already published the stop level.
    case MessageKind::soft_stop:
        serve_soft_stop(message.soft_stop);
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
                wake_owner();
            }
        } else {
            slot = message.port;
        }
        break;
    }
    }
}
void MotorExecution::drain_inbox(TickType_t wait_ticks) {
    Message message{};
    size_t drained = 0;
    if (wait_ticks != 0) {
        if (xQueueReceive(inbox_, &message, wait_ticks) == pdTRUE) {
            handle_message(message);
            drained = 1;
        } else {
            return;
        }
    }
    for (; drained < inbox_slots && xQueueReceive(inbox_, &message, 0) == pdTRUE; ++drained) {
        handle_message(message);
    }
}
void MotorExecution::worker_entry(void *context) {
    static_cast<MotorExecution *>(context)->run();
}
// This task never calls Core queries/step and never waits for the operator, touchscreen or storage.
void MotorExecution::run() {
    TickType_t wait_ticks = 0;
    for (;;) {
        drain_inbox(wait_ticks);
        wait_ticks = 1; // Wake immediately on inbox messages, bounded by 1 tick for PWM/stop poll.
        const auto now = uint64_t(esp_timer_get_time());
        if (last_cycle_us_) {
            cycle_gap_max_us_ = std::max(cycle_gap_max_us_, now - last_cycle_us_);
        }
        last_cycle_us_ = now;
        const auto rc = advance_owner();
        if (rc != AYNI_OK && rc != AYNI_BUSY) {
            accepting_.store(false, std::memory_order_release);
            stop_owner();
            worker_error_.store(rc, std::memory_order_release);
            wake_owner(); // The owner reports the failure and stops.
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
    const auto access = inbox_access_.fetch_or(inbox_closed, std::memory_order_acq_rel);
    accepting_.store(false, std::memory_order_release);
    if (!worker_) {
        if (!close_owner()) {
            return false;
        }
        release_worker_storage(); // Also releases a reserved stack when never prepared.
        return true;
    }
    closing_.store(true, std::memory_order_release);
    if ((access & ~inbox_closed) != 0U || !worker_stopped_.load(std::memory_order_acquire) ||
        eTaskGetState(worker_) != eSuspended) {
        return false;
    }
    // IDF6 legacy dual-CPU join: suspension excludes execution before freeing static memory.
    vTaskDelete(worker_);
    worker_ = nullptr;
    release_worker_storage();
    return true;
}
} // namespace wifichat
