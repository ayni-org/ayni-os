#include <ayni/runtime/native_node.h>

#include <ayni/hosts/native.h>

#include <cstdlib>
#include <new>

namespace ayni::runtime {

NativeNode::~NativeNode() {
    if (kernel_ != nullptr && release() != AYNI_OK) {
        std::abort();
    }
}

ayni_status_t NativeNode::assemble() {
    if (kernel_ != nullptr) {
        return AYNI_INVALID_STATE;
    }

    ayni_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;

    std::size_t size = 0;
    std::size_t align = 0;
    ayni_status_t status = ayni_kernel_memory_requirements(&config, &size, &align);
    if (status != AYNI_OK) {
        return status;
    }

    void *memory = ::operator new(size, std::align_val_t{align}, std::nothrow);
    if (memory == nullptr) {
        return AYNI_CAPACITY_EXCEEDED;
    }

    const ayni_host_v1 host = hosts::native::host_table();
    ayni_kernel_t *kernel = nullptr;
    status = ayni_create(&config, &host, memory, size, &kernel);
    if (status != AYNI_OK) {
        ::operator delete(memory, std::align_val_t{align});
        return status;
    }

    memory_ = memory;
    memory_align_ = align;
    kernel_ = kernel;
    return AYNI_OK;
}

ayni_status_t NativeNode::release() {
    if (kernel_ == nullptr) {
        return AYNI_INVALID_STATE;
    }
    const ayni_status_t status = ayni_destroy(kernel_);
    if (status != AYNI_OK) {
        return status;
    }
    ::operator delete(memory_, std::align_val_t{memory_align_});
    kernel_ = nullptr;
    memory_ = nullptr;
    memory_align_ = 0;
    return AYNI_OK;
}

} // namespace ayni::runtime
