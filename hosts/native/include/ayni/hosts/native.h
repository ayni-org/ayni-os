// Native Host for Linux and macOS builds: the platform services the Kernel
// uses to maintain its own invariants (ADR-0006): clock, wake and the ingress
// lock (ADR-0015).

#ifndef AYNI_HOSTS_NATIVE_H
#define AYNI_HOSTS_NATIVE_H

#include <ayni/ayni.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace ayni::hosts::native {

// Serves one Kernel. The table's ctx points at this object, so it is neither
// copyable nor movable and must outlive the Kernel (C-HOST-001).
class Host {
  public:
    Host() = default;
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // The clock is std::chrono::steady_clock, which never decreases. wake and
    // the ingress lock may be called from any thread.
    ayni_host_v1 table();

    // For the owner thread: returns true as soon as wake has been called since
    // the previous return, or false once timeout passes without a wake.
    bool wait_for_wake(std::chrono::microseconds timeout);

  private:
    static void wake(void *ctx);
    static void ingress_lock(void *ctx);
    static void ingress_unlock(void *ctx);

    std::mutex ingress_;
    std::mutex wake_mutex_;
    std::condition_variable wake_signal_;
    bool wake_pending_ = false;
};

} // namespace ayni::hosts::native

#endif // AYNI_HOSTS_NATIVE_H
