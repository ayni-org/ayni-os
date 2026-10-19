// Simulation Host for tests and the simulator (MA-003, MA-005): time moves
// only when the test advances it (FakeClock, C-TIME-001), wakes are counted
// instead of scheduling a thread, and misuse of the ingress lock is recorded
// so that tests can assert the kernel never calls the Host while holding it
// (C-HOST-020). The lock is a real mutex, so producer threads may be used.

#ifndef AYNI_HOSTS_SIM_H
#define AYNI_HOSTS_SIM_H

#include <ayni/ayni.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ayni::hosts::sim {

// Serves one Kernel. The table's ctx points at this object, so it is neither
// copyable nor movable and must outlive the Kernel (C-HOST-001).
class Host {
  public:
    explicit Host(uint64_t start_us = 0) : now_us_(start_us) {}
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    ayni_host_v1 table();

    // FakeClock. Owner thread, like monotonic_now_us itself.
    uint64_t now_us() const { return now_us_; }
    void advance_us(uint64_t delta_us) { now_us_ += delta_us; }

    // Any thread.
    uint64_t wake_count() const { return wake_count_.load(); }
    // Host calls made by a thread that held the ingress lock at the time.
    uint64_t calls_under_lock() const { return calls_under_lock_.load(); }

  private:
    static uint64_t monotonic_now_us(void *ctx);
    static void wake(void *ctx);
    static void ingress_lock(void *ctx);
    static void ingress_unlock(void *ctx);
    void note_call();

    uint64_t now_us_;
    std::atomic<uint64_t> wake_count_{0};
    std::atomic<uint64_t> calls_under_lock_{0};
    std::mutex ingress_;
    std::atomic<std::thread::id> holder_{};
};

} // namespace ayni::hosts::sim

#endif // AYNI_HOSTS_SIM_H
