// Composition root for native targets (ADR-0010): assembles one Kernel with
// the native Host, whose persistent log is a file in a state directory the
// caller chooses (MA-010). The node creates the Kernel in CONFIGURING; the
// owner thread then drives it through the C API, ayni_start included, and
// waits for the Host's wakes here. Registration and the CORE_FULL capacity
// profile arrive with the work packages that implement them.

#ifndef AYNI_RUNTIME_NATIVE_NODE_H
#define AYNI_RUNTIME_NATIVE_NODE_H

#include <ayni/ayni.h>
#include <ayni/bodies.h>
#include <ayni/runtime/common/clock.h>

#include <chrono>
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
    // Releases an assembled Kernel. If release() refuses (the owner skipped
    // shutdown), the memory and the Host are left allocated, and the Host's
    // I/O thread running, instead of being freed under a live Kernel;
    // callers that need the outcome call release().
    ~NativeNode();

    NativeNode(const NativeNode &) = delete;
    NativeNode &operator=(const NativeNode &) = delete;

    // Allocates the Host, opens its log in state_directory, which must exist
    // and is used by one node at a time, and creates the Kernel. Returns
    // AYNI_INVALID_STATE if already assembled; AYNI_INVALID_ARGUMENT if
    // state_directory is NULL; AYNI_CAPACITY_EXCEEDED if an allocation or the
    // Host's I/O thread fails; AYNI_BUSY if another Host has the log open;
    // AYNI_UNSUPPORTED_VERSION if the log file is not in the Host's format;
    // AYNI_STORAGE_FAILED if a file call fails. Other failures come from the
    // C API unchanged. On failure the node holds nothing.
    ayni_status_t assemble(const char *state_directory);

    // Explicit deployment capacities; copied by create, with identical ownership
    // and failures to assemble(directory). The caller registers its profile.
    // bodies, when given, is where Core releases the bodies its records stop naming
    // (ADR-0096); it is borrowed until release() returns AYNI_OK.
    ayni_status_t assemble(const char *state_directory, const ayni_config_v1 &config,
                           const ayni_body_store_v1 *bodies = nullptr);

    // Owner thread, between ayni_step calls: true as soon as the Host's wake
    // was called since the previous return, false once timeout passes
    // without one, or at once when nothing is assembled.
    bool wait_for_wake(std::chrono::microseconds timeout);

    // Stops the Host's I/O thread, destroys the Kernel, then frees its memory
    // and the Host. Returns AYNI_INVALID_STATE, changing nothing, if nothing
    // is assembled or the Kernel is neither CONFIGURING nor STOPPED (call
    // ayni_begin_shutdown and step it to STOPPED first); a failure from
    // ayni_destroy is returned unchanged and the node keeps the Kernel, its
    // storage closed.
    ayni_status_t release();

    // Null unless assembled. Valid until release() returns AYNI_OK.
    ayni_kernel_t *kernel() const { return kernel_; }
    Clock clock() const; // Same Host clock ports, borrowed until release.

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
