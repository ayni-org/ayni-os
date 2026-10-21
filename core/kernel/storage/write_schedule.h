// One monotonic write budget shared by journal, recovery and maintenance.
// It schedules calls; it never waits or synthesizes Host completions.
#ifndef AYNI_CORE_KERNEL_STORAGE_WRITE_SCHEDULE_H
#define AYNI_CORE_KERNEL_STORAGE_WRITE_SCHEDULE_H

#include <cstdint>
#include <limits>

namespace ayni::kernel {

inline uint64_t storage_after(uint64_t now, uint64_t delay) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    return delay > max - now ? max : now + delay;
}

inline uint64_t storage_min(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}
inline uint64_t storage_max(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

class WriteSchedule {
  public:
    explicit WriteSchedule(uint32_t interval_us = 10000) : interval_us_(interval_us) {}
    bool ready(uint64_t now) const { return now >= next_us_; }
    uint64_t deadline() const { return next_us_; }
    void started(uint64_t now) { next_us_ = storage_after(now, interval_us_); }

  private:
    uint64_t interval_us_;
    uint64_t next_us_ = 0;
};

// Attempts belong to one immutable encoded batch. A new request id is still
// allocated by CompletionPool for every attempt.
class AppendRetry {
  public:
    void reset() {
        attempts_ = 0;
        due_us_ = 0;
    }
    void started() { ++attempts_; }
    bool failed(uint64_t now) {
        if (attempts_ >= 3U) {
            return false;
        }
        due_us_ = storage_after(now, attempts_ == 1U ? 10000U : 50000U);
        return true;
    }
    bool retrying() const { return due_us_ != 0U; }
    bool ready(uint64_t now) const { return now >= due_us_; }
    uint64_t deadline() const { return due_us_; }

  private:
    uint32_t attempts_ = 0;
    uint64_t due_us_ = 0;
};

} // namespace ayni::kernel
#endif
