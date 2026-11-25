// Host-only wall-clock measurement; simulated timestamps never stand in for CPU latency.
#ifndef AYNI_TEST_LATENCY_SAMPLES_H
#define AYNI_TEST_LATENCY_SAMPLES_H
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
namespace ayni::test {
// Debug and sanitizer builds (especially under parallel CI) carry 10-30x instrumentation
// overhead and OS scheduling preemption spikes; strict microbenchmark P99 gates apply to
// optimized non-sanitizer builds while instrumented builds still check coarse hang ceilings.
inline constexpr bool is_sanitized_or_debug() {
#if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}
class LatencySamples {
  public:
    using Clock = std::chrono::steady_clock;
    void add(Clock::time_point start) {
        values_.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    void append(const LatencySamples &other) {
        values_.insert(values_.end(), other.values_.begin(), other.values_.end());
    }
    double percentile(unsigned percent) const {
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        return sorted.empty() ? 0 : sorted[(sorted.size() - 1) * percent / 100];
    }
    size_t size() const { return values_.size(); }
    void print(const char *name) const {
        std::printf("host_latency %s n=%zu p50_us=%.3f p99_us=%.3f max_us=%.3f\n", name, size(),
                    percentile(50), percentile(99), percentile(100));
    }

  private:
    std::vector<double> values_;
};
} // namespace ayni::test
#endif
