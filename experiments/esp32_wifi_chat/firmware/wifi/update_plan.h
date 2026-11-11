#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
namespace wifichat::wifi {
// Caller serializes access. This owns update progress and the shutdown/reply barriers,
// not HTTP, flash, Core, credentials or device execution.
class UpdatePlan {
  public:
    enum class Phase {
        idle,
        requested,
        closing,
        receiving,
        verifying,
        committed,
        rejected,
        aborting_close,
        restore
    };
    struct Snapshot {
        Phase phase = Phase::idle;
        size_t received = 0, total = 0;
        int error = 0;
    };
    bool request(size_t total, size_t maximum) {
        if ((state_.phase != Phase::idle && state_.phase != Phase::rejected) || !total ||
            total > maximum) {
            return false;
        }
        state_ = {Phase::requested, 0, total, 0};
        reply_ms_.reset();
        return true;
    }
    bool close() {
        if (state_.phase != Phase::requested) {
            return false;
        }
        state_.phase = Phase::closing;
        return true;
    }
    // UI may refuse before it successfully enqueues shutdown; no close is then in flight.
    void close_refused(int error) {
        if (state_.phase == Phase::requested || state_.phase == Phase::closing ||
            state_.phase == Phase::aborting_close) {
            state_.phase = Phase::rejected;
            state_.error = error;
        }
    }
    void stopped() {
        if (state_.phase == Phase::closing) {
            state_.phase = Phase::receiving;
        } else if (state_.phase == Phase::aborting_close) {
            state_.phase = Phase::restore;
        }
    }
    bool append(size_t size) {
        if (state_.phase != Phase::receiving || size > state_.total - state_.received) {
            return false;
        }
        state_.received += size;
        return true;
    }
    bool verify() {
        if (state_.phase != Phase::receiving || state_.received != state_.total) {
            return false;
        }
        state_.phase = Phase::verifying;
        return true;
    }
    bool commit() {
        if (state_.phase != Phase::verifying) {
            return false;
        }
        state_.phase = Phase::committed;
        return true;
    }
    void fail(int error) {
        state_.error = error;
        if (state_.phase == Phase::closing) {
            state_.phase = Phase::aborting_close;
        } else if (state_.phase == Phase::receiving || state_.phase == Phase::verifying) {
            state_.phase = Phase::restore;
        } else if (state_.phase == Phase::requested) {
            state_.phase = Phase::rejected;
        }
    }
    void replied(uint64_t now_ms) { reply_ms_ = now_ms; }
    bool reboot_due(uint64_t now_ms) const {
        constexpr uint64_t response_grace_ms = 1000;
        return (state_.phase == Phase::committed || state_.phase == Phase::restore) && reply_ms_ &&
               now_ms >= *reply_ms_ && now_ms - *reply_ms_ >= response_grace_ms;
    }
    Snapshot snapshot() const { return state_; }

  private:
    Snapshot state_{};
    std::optional<uint64_t> reply_ms_; // Present only after the HTTP reply was attempted.
};
} // namespace wifichat::wifi
