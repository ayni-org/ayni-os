#pragma once
#include <cstdint>
namespace wifichat::wifi {
// UI intent and observed control state are distinct: queued enable/disable is never reported
// as actual output permission. Stop/failure stays latched until this boot's UI is destroyed.
class MotorSwitch {
  public:
    enum class State { initializing, disabled, enabling, enabled, disabling, stopped, failed };
    State state() const { return state_; }
    bool desired() const { return desired_; }
    bool active() const { return state_ == State::enabled || state_ == State::enabling; }
    void initialized(bool ok) {
        if (!ok) {
            desired_ = false;
            state_ = State::failed;
        } else if (state_ == State::initializing) {
            state_ = State::disabled;
        }
    }
    void observe(uint32_t mode) {
        if (state_ == State::stopped || state_ == State::failed) {
            return;
        }
        if (mode == 0) {
            state_ = State::disabled;
        } else if (mode == 1) {
            state_ = desired_ ? State::enabled : State::disabling;
        } else {
            stop();
        }
    }
    void enable() {
        if (state_ == State::disabled) {
            desired_ = true;
        }
    }
    void queued() { state_ = State::enabling; }
    void disable() {
        desired_ = false;
        state_ = State::disabling;
    }
    void stop() {
        desired_ = false;
        state_ = State::stopped;
    }

  private:
    State state_ = State::initializing;
    bool desired_ = true; // User's latest choice: automatic resource initialization and default on.
};
} // namespace wifichat::wifi
