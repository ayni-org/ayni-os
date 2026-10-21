// Pacing of the storage requests that may write the medium (ADR-0027
// decisions 3 and 9). One WriteSchedule is shared by the commit log, the
// boot sequence and snapshot maintenance, so retries, epoch installs, boot
// records, snapshot pages, activations and trims together start at most one
// request per interval; reads are not paced. Both classes only say when a
// request may start: the owner thread never waits, it reports the time as
// the step's next deadline, and nothing here completes a request.
#ifndef AYNI_CORE_KERNEL_STORAGE_WRITE_SCHEDULE_H
#define AYNI_CORE_KERNEL_STORAGE_WRITE_SCHEDULE_H

#include <ayni/ayni.h>

#include <cstdint>
#include <limits>

namespace ayni::kernel {

// now + delay, saturating: a deadline past the clock's range never comes.
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
    // interval_us is the configured storage_write_interval_us (ayni.h).
    explicit WriteSchedule(uint32_t interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT)
        : interval_us_(interval_us) {}
    // A writing request may start at now.
    bool ready(uint64_t now) const { return now >= next_us_; }
    // When the next one may start; 0 until the first has started.
    uint64_t deadline() const { return next_us_; }
    // A writing request started at now, accepted by the Host or not.
    void started(uint64_t now) { next_us_ = storage_after(now, interval_us_); }

  private:
    uint64_t interval_us_;
    uint64_t next_us_ = 0;
};

// The bounded retry of one request (ADR-0027 decision 3). The request is
// immutable between attempts: the same batch identity and bytes, or the same
// snapshot page, so an attempt the Host did store is confirmed, not stored
// twice. Each attempt still takes a new request id from CompletionPool. An
// attempt is counted when it is issued, so a Host refusal counts and a
// request waiting for a free completion slot does not.
class AppendRetry {
  public:
    static constexpr uint32_t attempts_max = 3;
    // The wait after the first and the second failed attempt.
    static constexpr uint64_t first_backoff_us = 10000;
    static constexpr uint64_t second_backoff_us = 50000;

    // The request succeeded, or a new one begins.
    void reset() {
        attempts_ = 0;
        due_us_ = 0;
    }
    void started() { ++attempts_; }
    // The attempt started last failed at now. Returns whether another one
    // follows, once ready(); false when all attempts_max are spent.
    bool failed(uint64_t now) {
        if (attempts_ >= attempts_max) {
            return false;
        }
        due_us_ = storage_after(now, attempts_ == 1U ? first_backoff_us : second_backoff_us);
        return true;
    }
    // An attempt failed and none has succeeded since. The commit log admits
    // no new transition meanwhile (ADR-0027 decision 4).
    bool retrying() const { return due_us_ != 0U; }
    bool ready(uint64_t now) const { return now >= due_us_; }
    uint64_t deadline() const { return due_us_; }

  private:
    uint32_t attempts_ = 0;
    uint64_t due_us_ = 0;
};

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_STORAGE_WRITE_SCHEDULE_H
