#include <atomic>
#include <ayni/hosts/sim.h>
#include <ayni/runtime/sim_node.h>
#include <new>
namespace ayni::runtime {
struct SimNode::Impl {
    hosts::sim::Host host;
    ayni_kernel_t *kernel = nullptr;
    void *memory = nullptr;
    size_t alignment = 0;
    // Host itself stays owner-thread-only. These atomic copies expose the same
    // clock to the presentation worker; UTC packs seconds and trust in one read.
    std::atomic<uint64_t> public_now{0}, public_utc{0};
    static constexpr uint32_t trust_bits = 2;
    static constexpr uint64_t trust_mask = 3;
    static uint64_t now(void *context) { return static_cast<Impl *>(context)->public_now.load(); }
    static void utc(void *context, ayni_utc_sample_v1 *out) {
        const auto packed = static_cast<Impl *>(context)->public_utc.load();
        *out = {sizeof(*out), static_cast<ayni_utc_trust_t>(packed & trust_mask),
                static_cast<int64_t>(packed >> trust_bits)};
    }
};
SimNode::SimNode() = default;
SimNode::~SimNode() {
    if (impl_ && impl_->kernel && release() != AYNI_OK) {
        // Outstanding Kernel borrows own these lifetimes until explicit drain.
        static_cast<void>(impl_.release());
    }
}
ayni_status_t SimNode::assemble(const ayni_config_v1 &config) {
    if (!impl_) {
        impl_.reset(new (std::nothrow) Impl);
    }
    if (!impl_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (impl_->kernel) {
        return AYNI_INVALID_STATE;
    }
    size_t bytes = 0;
    auto status = ayni_kernel_memory_requirements(&config, &bytes, &impl_->alignment);
    if (status != AYNI_OK) {
        return status;
    }
    impl_->memory = ::operator new(bytes, std::align_val_t{impl_->alignment}, std::nothrow);
    if (!impl_->memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto host = impl_->host.table();
    status = ayni_create(&config, &host, impl_->memory, bytes, &impl_->kernel);
    if (status != AYNI_OK) {
        ::operator delete(impl_->memory, std::align_val_t{impl_->alignment});
        impl_->memory = nullptr;
    }
    return status;
}
void SimNode::advance(uint64_t microseconds) {
    if (impl_) {
        impl_->host.complete_storage();
        impl_->host.advance_us(microseconds);
        impl_->public_now.store(impl_->host.now_us());
    }
}
void SimNode::set_utc(int64_t seconds, ayni_utc_trust_t trust) {
    if (impl_) {
        impl_->host.set_utc(seconds, trust);
        const auto packed =
            seconds >= 0 && seconds <= AYNI_CALENDAR_SECONDS_MAX && trust <= AYNI_UTC_TRUSTED
                ? (static_cast<uint64_t>(seconds) << Impl::trust_bits) | trust
                : 0;
        impl_->public_utc.store(packed);
    }
}
ayni_status_t SimNode::release() {
    if (!impl_ || !impl_->kernel) {
        return AYNI_INVALID_STATE;
    }
    const auto status = ayni_destroy(impl_->kernel);
    if (status != AYNI_OK) {
        return status;
    }
    ::operator delete(impl_->memory, std::align_val_t{impl_->alignment});
    impl_->memory = nullptr;
    impl_->kernel = nullptr;
    return AYNI_OK;
}
hosts::sim::Host *SimNode::simulation() {
    return impl_ ? &impl_->host : nullptr;
}
const hosts::sim::Host *SimNode::simulation() const {
    return impl_ ? &impl_->host : nullptr;
}
ayni_status_t SimNode::power_cut(hosts::sim::CutAppend append) {
    if (!impl_ || !impl_->kernel) {
        return AYNI_INVALID_STATE;
    }
    // The simulated process cannot run cleanup or receive I/O after power loss.
    // Host stops borrowing first; clients have already been quiesced by the root.
    impl_->host.power_cut(append);
    ::operator delete(impl_->memory, std::align_val_t{impl_->alignment});
    impl_->memory = nullptr;
    impl_->kernel = nullptr;
    return AYNI_OK;
}
ayni_kernel_t *SimNode::kernel() const {
    return impl_ ? impl_->kernel : nullptr;
}
Clock SimNode::clock() const {
    if (!impl_) {
        return {};
    }
    return {impl_.get(), Impl::now, Impl::utc};
}
} // namespace ayni::runtime
