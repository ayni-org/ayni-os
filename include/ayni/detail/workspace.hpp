// Bounded synchronous temporary objects. The caller owns bytes and their placement;
// this owner tracks only nested lifetimes, never domain state or asynchronous work.
#ifndef AYNI_DETAIL_WORKSPACE_HPP
#define AYNI_DETAIL_WORKSPACE_HPP

#include <ayni/workspace.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace ayni::detail {

// Owns only a synchronous byte cursor; the caller retains the backing storage.
class Workspace {
  public:
    // Current portable temporaries need at most the widest integer's alignment.
    static constexpr std::size_t alignment = alignof(uint64_t);

    // A lexical lease destroys and erases its temporary before restoring the mark.
    // Leases are deliberately neither copied nor moved; C++17 elides returns.
    template <typename T> class Object {
      public:
        // Internal construction; only make publishes leases.
        Object(Workspace &owner, std::size_t mark, T *value)
            : owner_(owner), mark_(mark), value_(value) {}
        Object(const Object &) = delete;
        Object &operator=(const Object &) = delete;
        ~Object() {
            if (value_ != nullptr) {
                value_->~T();
                owner_.release(mark_);
            }
        }
        // False is a capacity refusal; dereference only a successful lease.
        explicit operator bool() const { return value_ != nullptr; }
        // Typed reference expires before this lexical lease is destroyed.
        T &operator*() const { return *value_; }
        // Typed pointer borrows the same lexical lifetime.
        T *operator->() const { return value_; }

      private:
        Workspace &owner_; // Borrowed owner; outlives every nested lease.
        std::size_t mark_; // Previous byte cursor; restored in reverse lexical order.
        T *value_;         // Null exactly when capacity was insufficient.
    };

    // Borrows aligned bytes through every lease; construction allocates nothing.
    Workspace(void *data, std::size_t bytes)
        : data_(static_cast<uint8_t *>(data)), capacity_(bytes) {}
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;

    // A public C operation borrows a disjoint byte lane, including through its
    // callbacks. Its descriptor expires with this lease, before the next reuse.
    class Region {
      public:
        // Internal construction admits the complete byte lane before advancing.
        Region(Workspace &owner, std::size_t bytes) : Region(owner, bytes, Cleanup::caller) {}
        Region(const Region &) = delete;
        Region &operator=(const Region &) = delete;
        ~Region() {
            if (view_.data != nullptr) {
                if (cleanup_ == Cleanup::caller) {
                    owner_.release(mark_);
                } else {
                    owner_.used_ = mark_; // The synchronous codec already erased its own scratch.
                }
            }
        }
        // False means no lane was reserved.
        explicit operator bool() const { return view_.data != nullptr; }
        // Descriptor and bytes both expire with this lease; null after refusal.
        const ayni_workspace_v1 *get() const { return view_.data ? &view_ : nullptr; }

      private:
        friend class Workspace;
        enum class Cleanup { caller, callee };
        Region(Workspace &owner, std::size_t bytes, Cleanup cleanup)
            : owner_(owner), mark_(owner.used_), cleanup_(cleanup) {
            const auto padding = (alignment - owner.used_ % alignment) % alignment;
            if (owner.data_ != nullptr && owner.used_ <= owner.capacity_ &&
                padding <= owner.capacity_ - owner.used_ && bytes != 0 &&
                bytes <= owner.capacity_ - owner.used_ - padding) {
                view_ = {sizeof(view_), 0, owner.data_ + owner.used_ + padding, bytes};
                owner.used_ += padding + bytes;
            }
        }
        Workspace &owner_; // Borrowed lexical arena; outlives this region.
        std::size_t mark_; // Byte cursor restored after the last synchronous borrower returns.
        Cleanup cleanup_;  // Callee only for an operation that clears all scratch it writes.
        ayni_workspace_v1
            view_{}; // Null data means no capacity; otherwise complete exclusive lane.
    };

    // Dynamic-size lanes bridge public memory queries without copying private layouts.
    Region take(std::size_t bytes) { return Region(*this, bytes); }

    // Delegate erasure only to a synchronous codec that clears every written scratch byte
    // on all returns (C-SDK-007). The caller must not write directly through this lane;
    // arbitrary callbacks still use take()/with_workspace and caller-owned erasure.
    Region delegate(std::size_t bytes) { return Region(*this, bytes, Region::Cleanup::callee); }

    // Constructs only after full capacity admission; rejection changes no cursor.
    template <typename T, typename... Args> Object<T> make(Args &&...args) {
        static_assert(alignof(T) <= alignment, "workspace supports this temporary's alignment");
        const auto padding = (alignof(T) - used_ % alignof(T)) % alignof(T);
        if (data_ == nullptr || used_ > capacity_ || padding > capacity_ - used_ ||
            sizeof(T) > capacity_ - used_ - padding) {
            return Object<T>(*this, used_, nullptr);
        }
        const auto mark = used_;
        auto *value = new (data_ + used_ + padding) T(std::forward<Args>(args)...);
        used_ += padding + sizeof(T);
        return Object<T>(*this, mark, value);
    }

    // Public-region preflight is shared by all synchronous C entry points.
    static ayni_status_t check(const ayni_workspace_v1 *region, std::size_t required) {
        if (region != nullptr && region->struct_size > sizeof(*region)) {
            return AYNI_UNSUPPORTED_VERSION;
        }
        if (region == nullptr || region->struct_size < sizeof(*region) || region->reserved != 0 ||
            region->data == nullptr ||
            reinterpret_cast<std::uintptr_t>(region->data) % alignment != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        return region->byte_capacity < required ? AYNI_BUFFER_TOO_SMALL : AYNI_OK;
    }

  private:
    // Erase private bytes before reuse. A volatile function-pointer load keeps
    // the call observable to optimizers without instrumenting each byte store.
    // Every successful lease owns at least one byte; no null/empty memset occurs.
    void release(std::size_t mark) {
        using Erase = void *(*)(void *, int, std::size_t);
        Erase volatile erase = &std::memset;
        erase(data_ + mark, 0, used_ - mark);
        used_ = mark;
    }

    uint8_t *data_;        // Borrowed base, aligned to alignment; null for disabled storage.
    std::size_t capacity_; // Bytes available from the base; zero disables allocation.
    std::size_t used_ = 0; // Byte cursor owned by nested leases, zero while idle.
};

// A queried public codec receives a disjoint lane; its failure remains classified.
// The operation is synchronous and may borrow other lanes through this arena.
template <typename Operation>
ayni_status_t with_workspace(Workspace &workspace, ayni_status_t (*query)(size_t *, size_t *),
                             Operation operation) {
    size_t bytes = 0, alignment = 0;
    const auto status = query(&bytes, &alignment);
    if (status != AYNI_OK) {
        return status;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > Workspace::alignment) {
        return AYNI_UNSUPPORTED;
    }
    auto region = workspace.take(bytes);
    return region ? operation(region.get()) : AYNI_CAPACITY_EXCEEDED;
}
} // namespace ayni::detail
#endif
