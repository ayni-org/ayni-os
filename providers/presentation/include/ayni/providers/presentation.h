// Bounded async notification channel. Core callbacks copy/enqueue; one worker writes.
#ifndef AYNI_PROVIDERS_PRESENTATION_H
#define AYNI_PROVIDERS_PRESENTATION_H
#include <ayni/calls.h>
#include <memory>
namespace ayni::providers::presentation {
inline constexpr uint32_t request_capacity = 8, text_bytes_max = 512;
// Called only by the writer worker, without a Provider lock. OK proves complete
// write+flush. UNSUPPORTED explicitly proves no channel/no output began; every
// other failure has UNKNOWN extent. Must not throw. Context lives through quiesce.
struct Output {
    void *context = nullptr;
    ayni_status_t (*write)(void *, ayni_string_v1) = nullptr;
};
// Bounded, thread-safe current window check on the writer worker. No I/O wait.
struct Availability {
    void *context = nullptr;
    ayni_status_t (*read)(void *, bool *) = nullptr;
};
class Provider {
  public:
    Provider();
    // Retains its context if the caller skipped drain; never frees a live completion borrow.
    ~Provider();
    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    // Allocates bounded slots and starts one worker before registration. Retain
    // the Provider through Kernel destruction; partial failure leaves it unconfigured.
    ayni_status_t configure(Output, Availability = {});
    ayni_provider_v1 describe();
    ayni_status_t advance();  // Owner-thread retry signal/worker status; performs no channel I/O.
    void set_available(bool); // Rechecked by worker immediately before first write.
    uint32_t pending() const;
    // STOPPED Kernel: join producer after its final post returned; retains descriptors.
    ayni_status_t quiesce();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::providers::presentation
#endif
