// Explicit temporary-memory owner for SDK consumers. Allocation happens before
// registration/dispatch; Core and strategy callbacks only borrow its descriptor.
#ifndef AYNI_WORKSPACE_HPP
#define AYNI_WORKSPACE_HPP
#include <ayni/workspace.h>
#include <new>
#include <optional>
#include <utility>

namespace ayni {
// Owns one startup allocation, or explicitly borrows caller-selected storage.
class Workspace {
  public:
    // Selects the query contract, never the platform or a runtime fallback.
    enum class Purpose { value, business };
    Workspace() = default;
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;
    Workspace(Workspace &&other) noexcept : allocation_(other.allocation_) {
        other.allocation_.reset();
    }
    Workspace &operator=(Workspace &&) = delete; // Never overwrite live borrowed storage.
    ~Workspace() { close(); }

    // Allocates exactly one queried, bounded region; failure leaves this owner empty.
    ayni_status_t create(Purpose purpose) {
        if (purpose != Purpose::value && purpose != Purpose::business) {
            return AYNI_INVALID_ARGUMENT;
        }
        return create(purpose == Purpose::value ? ayni_value_workspace_requirements
                                                : ayni_business_workspace_requirements);
    }
    // Component queries share the same explicit startup allocation and failure path.
    ayni_status_t create(ayni_status_t (*query)(size_t *, size_t *)) {
        if (query == nullptr) {
            return AYNI_INVALID_ARGUMENT;
        }
        size_t bytes = 0, alignment = 0;
        const auto status = query(&bytes, &alignment);
        return status == AYNI_OK ? create(bytes, alignment) : status;
    }
    // Also serves a component-specific memory query, e.g. ToolLoop scratch.
    ayni_status_t create(size_t bytes, size_t alignment) {
        if (allocation_) {
            return AYNI_INVALID_STATE;
        }
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto actual_alignment = alignment < alignof(void *) ? alignof(void *) : alignment;
        void *data = ::operator new(bytes, std::align_val_t(actual_alignment), std::nothrow);
        if (data == nullptr) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        allocation_ = Allocation{
            {sizeof(ayni_workspace_v1), 0, data, bytes}, actual_alignment, Ownership::allocated};
        return AYNI_OK;
    }
    // Borrows platform-selected memory. The caller retains it until close/destruction.
    ayni_status_t create_in(void *data, size_t bytes, size_t alignment) {
        if (allocation_) {
            return AYNI_INVALID_STATE;
        }
        if (data == nullptr || bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
            reinterpret_cast<uintptr_t>(data) % alignment != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        allocation_ =
            Allocation{{sizeof(ayni_workspace_v1), 0, data, bytes}, 0, Ownership::borrowed};
        return AYNI_OK;
    }
    // Stable only while this owner stays alive and unmoved; null before creation.
    const ayni_workspace_v1 *get() const { return allocation_ ? &allocation_->view : nullptr; }
    // Synchronous users must have returned; borrowed memory is never freed here.
    void close() {
        if (allocation_) {
            if (allocation_->ownership == Ownership::allocated) {
                ::operator delete(allocation_->view.data, std::align_val_t(allocation_->alignment));
            }
            allocation_.reset();
        }
    }

  private:
    enum class Ownership { borrowed, allocated };
    // One live allocation record; all fields become absent together on close/move.
    struct Allocation {
        ayni_workspace_v1 view; // Complete borrowed descriptor, valid exactly while record exists.
        size_t alignment;       // Allocation alignment in bytes; zero exactly for borrowed memory.
        Ownership ownership;    // Selects destruction; never inferred from a pointer or byte size.
    };
    std::optional<Allocation> allocation_; // Sole ownership record; empty before successful create.
};
} // namespace ayni
#endif
