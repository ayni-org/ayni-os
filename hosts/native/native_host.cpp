#include <ayni/hosts/native.h>

#include <cstdint>

namespace ayni::hosts::native {
namespace {

uint64_t monotonic_now_us(void * /*ctx*/) {
    const auto since_origin = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_origin).count());
}

} // namespace

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &monotonic_now_us;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    return host;
}

bool Host::wait_for_wake(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    const bool woken = wake_signal_.wait_for(lock, timeout, [this] { return wake_pending_; });
    wake_pending_ = false;
    return woken;
}

void Host::wake(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    {
        const std::lock_guard<std::mutex> lock(self.wake_mutex_);
        self.wake_pending_ = true;
    }
    self.wake_signal_.notify_one();
}

void Host::ingress_lock(void *ctx) {
    static_cast<Host *>(ctx)->ingress_.lock();
}

void Host::ingress_unlock(void *ctx) {
    static_cast<Host *>(ctx)->ingress_.unlock();
}

} // namespace ayni::hosts::native
