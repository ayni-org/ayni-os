// Pure background-write pacing predicate for ESP32 Wi-Fi Chat.
// Decides whether FileBodies should defer non-blocking persistence/cleanup during an active turn.
// Kept header-only and side-effect-free so firmware and host contract checks share one rule.
#pragma once
#include <ayni/ayni.h>
#include <ayni/calls.h>
#include <cstdint>

namespace wifichat {

// Experimental pre-stop guard band: stop submitting new background Flash writes this long before
// an active motor procedure's expected completion so an in-flight LittleFS write has room to
// drain before the motor stops. Never delays motor output or stopping.
inline constexpr uint64_t motor_write_guard_us = 800000;

struct WritePacingInput {
    ayni_task_lifecycle_t task_lifecycle = 0; // Current active Task lifecycle from Core.
    bool has_active_call = false;         // True when foreground/waiting Call info is populated.
    ayni_call_outcome_t call_outcome = 0; // Active Call outcome; zero while pending.
    ayni_call_wait_t call_wait = AYNI_CALL_WAIT_NONE; // Active Call wait reason from Core.
    uint64_t now_us = 0;                              // Current monotonic host timestamp.
    uint64_t motor_expected_stop_us = 0; // Estimated active motor stop timestamp; zero when idle.
};

// Returns true when background FileBodies writes should stay paused for this active turn tick.
// Unpauses on terminal/held Tasks, stalled resources, and during an active motor procedure whose
// remaining time exceeds motor_write_guard_us. Keeps HTTP/unbounded Provider waits paused until
// separately measured so an in-flight write does not spill across a short stream tail into before.
inline bool should_pause_background_writes(const WritePacingInput &in) {
    if (in.task_lifecycle == AYNI_TASK_READY) {
        return true;
    }
    if (in.task_lifecycle != AYNI_TASK_RUNNING && in.task_lifecycle != AYNI_TASK_WAITING) {
        return false;
    }
    if (!in.has_active_call) {
        return in.task_lifecycle == AYNI_TASK_RUNNING;
    }
    if (in.call_outcome != 0U) {
        return in.call_wait == AYNI_CALL_WAIT_DURABILITY;
    }
    if (in.call_wait == AYNI_CALL_WAIT_NONE || in.call_wait == AYNI_CALL_WAIT_DURABILITY) {
        return true;
    }
    if (in.call_wait == AYNI_CALL_WAIT_PROVIDER) {
        const bool motor_has_guard_headroom =
            in.motor_expected_stop_us > in.now_us &&
            (in.motor_expected_stop_us - in.now_us) > motor_write_guard_us;
        return !motor_has_guard_headroom;
    }
    return false;
}

} // namespace wifichat
