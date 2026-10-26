#include <ayni/hosts/sim.h>
#include <ayni/runtime/sim_node.h>
#include <new>
namespace ayni::runtime {
struct SimNode::Impl {
    hosts::sim::Host host;
    ayni_kernel_t *kernel = nullptr;
    void *memory = nullptr;
    size_t alignment = 0;
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
ayni_kernel_t *SimNode::kernel() const {
    return impl_ ? impl_->kernel : nullptr;
}
} // namespace ayni::runtime
