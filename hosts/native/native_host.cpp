#include <ayni/hosts/native.h>

#include <chrono>
#include <cstdint>

namespace ayni::hosts::native {
namespace {

uint64_t monotonic_now_us(void * /*ctx*/) {
    const auto since_origin = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_origin).count());
}

} // namespace

ayni_host_v1 host_table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = nullptr;
    host.monotonic_now_us = &monotonic_now_us;
    return host;
}

} // namespace ayni::hosts::native
