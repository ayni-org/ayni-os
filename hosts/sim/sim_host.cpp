#include <ayni/hosts/sim.h>

#include <cstdio>
#include <cstdlib>

namespace ayni::hosts::sim {

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &Host::monotonic_now_us;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    return host;
}

uint64_t Host::monotonic_now_us(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    return self.now_us_;
}

void Host::wake(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.note_call();
    self.wake_count_.fetch_add(1);
}

void Host::ingress_lock(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    if (self.holder_.load() == std::this_thread::get_id()) {
        // A second lock on the same thread would deadlock; fail loudly instead.
        std::fputs("sim host: ingress lock taken twice by one thread\n", stderr);
        std::abort();
    }
    self.ingress_.lock();
    self.holder_.store(std::this_thread::get_id());
}

void Host::ingress_unlock(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    self.holder_.store(std::thread::id{});
    self.ingress_.unlock();
}

void Host::note_call() {
    if (holder_.load() == std::this_thread::get_id()) {
        calls_under_lock_.fetch_add(1);
    }
}

} // namespace ayni::hosts::sim
