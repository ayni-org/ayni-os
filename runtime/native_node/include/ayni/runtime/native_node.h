// Composition root for native targets (ADR-0010): assembles one Kernel with
// the native Host. In M0 the node only creates the Kernel, which stays in
// CONFIGURING; registration, ayni_start and the CORE_FULL capacity profile
// arrive with the APIs that implement them (M1).

#ifndef AYNI_RUNTIME_NATIVE_NODE_H
#define AYNI_RUNTIME_NATIVE_NODE_H

#include <ayni/ayni.h>

#include <cstddef>
#include <memory>

namespace ayni::hosts::native {
class Host;
}

namespace ayni::runtime {

// Owns the Kernel and its memory block; not copyable, so exactly one owner can
// release it. Used from one thread, which becomes the Kernel owner thread.
class NativeNode {
  public:
    NativeNode();
    // Releases an assembled Kernel. If ayni_destroy refuses (the owner
    // skipped shutdown), the memory and the Host are left allocated instead
    // of being freed under a live Kernel; callers that need the outcome call
    // release().
    ~NativeNode();

    NativeNode(const NativeNode &) = delete;
    NativeNode &operator=(const NativeNode &) = delete;

    // Allocates the Host and the Kernel memory and creates the Kernel.
    // Returns AYNI_INVALID_STATE if already assembled and
    // AYNI_CAPACITY_EXCEEDED if an allocation fails; other failures come from
    // the C API unchanged. On failure the node holds nothing.
    ayni_status_t assemble();

    // Destroys the Kernel, then frees its memory and the Host. Returns AYNI_INVALID_STATE if
    // nothing is assembled; a failure from ayni_destroy is returned unchanged
    // and the node keeps the Kernel.
    ayni_status_t release();

    // Null unless assembled. Valid until release() returns AYNI_OK.
    ayni_kernel_t *kernel() const { return kernel_; }

  private:
    // Kept out of this header so that targets depend on runtime/ only
    // (ADR-0010); outlives the Kernel it serves.
    std::unique_ptr<hosts::native::Host> host_;
    void *memory_ = nullptr;
    std::size_t memory_align_ = 0;
    ayni_kernel_t *kernel_ = nullptr;
};

} // namespace ayni::runtime

#endif // AYNI_RUNTIME_NATIVE_NODE_H
