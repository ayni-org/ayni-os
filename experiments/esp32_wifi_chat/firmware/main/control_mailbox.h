// Independent target/stop/feedback lanes; none borrows the UI packet queue (ADR-0114).
#pragma once
#include "latest_slot.h"
#include <algorithm>
#include <ayni/control.h>
#include <limits>

namespace wifichat {
enum class ControlRejectReason : uint32_t {
    none,
    no_active_session,
    out_of_envelope,
    stop_pending,
    disarmed_or_latched,
    interlock_fault
};
enum class ControlMode : uint8_t { disarmed, armed, latched, closed };
enum class ControlStopKind : uint32_t { soft_stop = 1, hard_estop = 2 };
enum class ControlTargetSource : uint8_t { structured = 1, authorized_model = 2 };
// A desired setpoint only; the executor still requires the live Core session envelope.
struct ControlTarget {
    uint64_t seq = 0, accepted_at_us = 0;
    int32_t motor1 = 0, motor2 = 0;
    uint32_t duration_ms = 0;
    ControlTargetSource source = ControlTargetSource::structured;
};
// Receipt and execution frontiers are independent: receiving never implies hardware application.
struct ControlFeedbackSnapshot {
    uint64_t accepted_seq = 0, superseded_seq = 0, superseded_count = 0;
    uint64_t applied_seq = 0, rejected_seq = 0, stop_requested_seq = 0, stop_confirmed_seq = 0;
    uint64_t accepted_at_us = 0, applied_at_us = 0, rejected_at_us = 0, stopped_at_us = 0;
    int32_t applied_motor1 = 0, applied_motor2 = 0;
    ControlRejectReason reject_reason = ControlRejectReason::none;
    ayni_status_t reject_status = AYNI_OK;
    uint32_t stop_cause = 0;
    uint8_t accepted_kind = 0, control_mode = 0, hold_active = 0, busy = 0;
};
struct ControlStopRequest {
    uint32_t seq = 0; // Also the target barrier: all older commands are refused, even after ACK.
    ControlStopKind kind = ControlStopKind::soft_stop;
};
struct ControlHold {
    uint32_t seq = 0, scale_percent = 100;
};
// Multiple non-ISR request producers; single executor and single UI snapshot reader.
// Sequence exhaustion refuses ordinary commands and permanently latches a hard stop.
class ControlMailbox {
  public:
    bool submit(ControlTarget target, uint64_t now_us, uint64_t *out_seq = nullptr) {
        uint32_t issued = 0;
        const bool sent = targets_.write(
            [&](ControlTarget &out) {
                issued = next_sequence();
                if (!issued) {
                    return false;
                }
                target.seq = issued;
                target.accepted_at_us = now_us;
                out = target;
                return true;
            },
            [&](const ControlTarget &previous, bool overwritten, const ControlTarget &accepted) {
                receipt_.accepted_seq = accepted.seq;
                receipt_.accepted_at_us = now_us;
                receipt_.accepted_kind = target_kind;
                if (overwritten) {
                    receipt_.superseded_seq = previous.seq;
                    ++receipt_.superseded_count;
                }
                receipts_.publish(
                    receipt_); // Serialized by the target writer, UI pins another slot.
            });
        if (sent && out_seq) {
            *out_seq = issued;
        }
        return sent;
    }
    bool take_target(ControlTarget &out) { return targets_.read(out); }
    bool target_blocked(uint64_t seq) const { return stop_pending() || seq <= stop_request().seq; }
    // No clear-on-read. An unconfirmed stop remains visible through any number of polls.
    ControlStopRequest request_stop(ControlStopKind kind) {
        const auto seq = next_sequence();
        auto state = stop_.load(std::memory_order_acquire);
        for (;;) {
            const auto severity = std::max(state & kind_mask, uint32_t(kind));
            const auto frontier = std::max(state >> stop_shift, seq ? seq : exhausted_sequence);
            const auto next = (frontier << stop_shift) | severity;
            if (stop_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return {frontier, ControlStopKind(severity)};
            }
        }
    }
    ControlStopRequest stop_request() const {
        const auto state = stop_.load(std::memory_order_acquire);
        return {state >> stop_shift, ControlStopKind(state & kind_mask)};
    }
    bool stop_pending() const {
        return confirmed_.load(std::memory_order_acquire) < stop_request().seq;
    }
    // Executor-only AFTER GPIO low readback and PermitGate STOPPED confirmation.
    // Confirming an earlier request never clears one that arrived during the hardware stop.
    void confirm_stop(uint32_t seq, bool armed, uint32_t cause, uint64_t stopped_us) {
        if (!seq || seq > stop_request().seq) {
            return;
        }
        execution_.stop_confirmed_seq = std::max<uint64_t>(execution_.stop_confirmed_seq, seq);
        execution_.stopped_at_us = stopped_us;
        execution_.stop_cause = cause;
        execution_.control_mode = armed ? uint8_t(ControlMode::armed) : execution_.control_mode;
        execution_.busy = 0;
        feedback_.publish(execution_);
        confirmed_.store(uint32_t(execution_.stop_confirmed_seq), std::memory_order_release);
    }
    bool request_hold(uint32_t percent) {
        if (percent > full_scale) {
            return false;
        }
        const auto seq = next_sequence();
        if (!seq) {
            return false;
        }
        auto state = hold_.load(std::memory_order_acquire);
        while ((state >> hold_shift) < seq) {
            if (hold_.compare_exchange_weak(state, (seq << hold_shift) | percent,
                                            std::memory_order_release, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }
    ControlHold hold() const {
        const auto state = hold_.load(std::memory_order_acquire);
        return {state >> hold_shift, state & hold_mask};
    }
    // Executor-only frontiers are retained in a separate triple buffer from producer receipts.
    void mark_applied(uint64_t seq, int32_t first, int32_t second, uint64_t at_us) {
        execution_.applied_seq = seq;
        execution_.applied_motor1 = first;
        execution_.applied_motor2 = second;
        execution_.applied_at_us = at_us;
        feedback_.publish(execution_);
    }
    void mark_rejected(uint64_t seq, ControlRejectReason reason, ayni_status_t code,
                       uint64_t at_us) {
        execution_.rejected_seq = seq;
        execution_.reject_reason = reason;
        execution_.reject_status = code;
        execution_.rejected_at_us = at_us;
        feedback_.publish(execution_);
    }
    void publish_mode(uint8_t mode, bool busy, bool held) {
        if (execution_.control_mode == mode && execution_.busy == busy &&
            execution_.hold_active == held) {
            return;
        }
        execution_.control_mode = mode;
        execution_.busy = busy;
        execution_.hold_active = held;
        feedback_.publish(execution_);
    }
    // UI-only; each copy pins a disjoint buffer, with no seqlock payload races.
    ControlFeedbackSnapshot snapshot() {
        feedback_.read(ui_execution_, false);
        receipts_.read(ui_receipt_, false);
        auto out = ui_execution_;
        out.accepted_seq = ui_receipt_.accepted_seq;
        out.accepted_at_us = ui_receipt_.accepted_at_us;
        out.accepted_kind = ui_receipt_.accepted_kind;
        out.superseded_seq = ui_receipt_.superseded_seq;
        out.superseded_count = ui_receipt_.superseded_count;
        const auto held = hold();
        const auto stop = stop_request();
        if (held.seq > out.accepted_seq) {
            out.accepted_seq = held.seq;
            out.accepted_kind = hold_kind;
            out.accepted_at_us = 0; // Atomic hold/stop lanes carry no timestamp payload.
        }
        if (stop.seq > out.accepted_seq) {
            out.accepted_seq = stop.seq;
            out.accepted_kind = stop.kind == ControlStopKind::hard_estop ? hard_kind : soft_kind;
            out.accepted_at_us = 0;
        }
        out.stop_requested_seq = stop.seq;
        return out;
    }

  private:
    static constexpr uint32_t stop_shift = 2, kind_mask = 3, hold_shift = 8, hold_mask = 255;
    static constexpr uint32_t exhausted_sequence = UINT32_MAX >> hold_shift,
                              max_sequence = exhausted_sequence - 1U, full_scale = 100;
    static constexpr uint8_t target_kind = 1, hold_kind = 2, soft_kind = 3, hard_kind = 4;
    uint32_t next_sequence() {
        auto current = sequence_.load(std::memory_order_relaxed);
        while (current < max_sequence) {
            if (sequence_.compare_exchange_weak(current, current + 1U, std::memory_order_relaxed)) {
                return current + 1U;
            }
        }
        stop_.store((exhausted_sequence << stop_shift) | uint32_t(ControlStopKind::hard_estop),
                    std::memory_order_release);
        return 0;
    }
    std::atomic<uint32_t> sequence_{0}, stop_{0}, confirmed_{0}, hold_{full_scale};
    LatestSlot<ControlTarget> targets_;
    LatestSlot<ControlFeedbackSnapshot> receipts_, feedback_;
    ControlFeedbackSnapshot receipt_{}, execution_{}; // Target writer / executor, respectively.
    ControlFeedbackSnapshot ui_receipt_{}, ui_execution_{}; // UI-owned cached snapshots.
};
} // namespace wifichat
