#include <ayni/runtime/native_node.h>

#include <ayni/hosts/native.h>
#include <ayni/runtime/common/default_config.h>

#include <new>
#include <utility>

namespace ayni::runtime {
namespace {

ayni_status_t storage_status(hosts::native::StorageStatus status) {
    switch (status) {
    case hosts::native::StorageStatus::ok:
        return AYNI_OK;
    case hosts::native::StorageStatus::invalid_argument:
        return AYNI_INVALID_ARGUMENT;
    case hosts::native::StorageStatus::already_open:
        return AYNI_INVALID_STATE;
    case hosts::native::StorageStatus::no_memory:
    case hosts::native::StorageStatus::thread_error:
        return AYNI_CAPACITY_EXCEEDED;
    case hosts::native::StorageStatus::in_use:
        return AYNI_BUSY;
    case hosts::native::StorageStatus::not_a_log:
        return AYNI_UNSUPPORTED_VERSION;
    case hosts::native::StorageStatus::file_error:
        break;
    }
    return AYNI_STORAGE_FAILED;
}

} // namespace

NativeNode::NativeNode() = default;

NativeNode::~NativeNode() {
    if (kernel_ != nullptr && release() != AYNI_OK) {
        // The Kernel still uses the block and the Host, so both are
        // deliberately left allocated; release() is where the failure is
        // reported.
        static_cast<void>(host_.release());
    }
}

ayni_status_t NativeNode::assemble(const char *state_directory) {
    return assemble(state_directory, default_config());
}

ayni_status_t NativeNode::assemble(const char *state_directory, const ayni_config_v1 &config) {
    if (kernel_ != nullptr) {
        return AYNI_INVALID_STATE;
    }

    std::size_t size = 0;
    std::size_t align = 0;
    ayni_status_t status = ayni_kernel_memory_requirements(&config, &size, &align);
    if (status != AYNI_OK) {
        return status;
    }

    std::unique_ptr<hosts::native::Host> host(new (std::nothrow) hosts::native::Host());
    if (host == nullptr) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    // Every request the Kernel can have in flight fits the Host's queue.
    status =
        storage_status(host->open_storage(state_directory, config.completion_slot_count).status);
    if (status != AYNI_OK) {
        return status;
    }
    void *memory = ::operator new(size, std::align_val_t{align}, std::nothrow);
    if (memory == nullptr) {
        return AYNI_CAPACITY_EXCEEDED;
    }

    const ayni_host_v1 table = host->table();
    ayni_kernel_t *kernel = nullptr;
    status = ayni_create(&config, &table, memory, size, &kernel);
    if (status != AYNI_OK) {
        ::operator delete(memory, std::align_val_t{align});
        return status;
    }

    host_ = std::move(host);
    memory_ = memory;
    memory_align_ = align;
    kernel_ = kernel;
    return AYNI_OK;
}

bool NativeNode::wait_for_wake(std::chrono::microseconds timeout) {
    return host_ != nullptr && host_->wait_for_wake(timeout);
}

ayni_status_t NativeNode::release() {
    if (kernel_ == nullptr) {
        return AYNI_INVALID_STATE;
    }
    // The I/O thread must be stopped before ayni_destroy, because its last
    // ayni_completion_post may still be returning (C-HOST-018). Stopping it
    // is safe only where ayni_destroy succeeds: in these two states no
    // request is in flight.
    ayni_lifecycle_t state = 0;
    ayni_status_t status = ayni_get_lifecycle(kernel_, &state);
    if (status != AYNI_OK) {
        return status;
    }
    if (state != AYNI_LIFECYCLE_CONFIGURING && state != AYNI_LIFECYCLE_STOPPED) {
        return AYNI_INVALID_STATE;
    }
    host_->close_storage();
    status = ayni_destroy(kernel_);
    if (status != AYNI_OK) {
        return status;
    }
    ::operator delete(memory_, std::align_val_t{memory_align_});
    host_.reset();
    kernel_ = nullptr;
    memory_ = nullptr;
    memory_align_ = 0;
    return AYNI_OK;
}

} // namespace ayni::runtime
