// Deterministic single-owner composition root. The in-memory Host survives a
// clean Kernel release/reassemble, so fixtures can continue on the same storage.
#ifndef AYNI_RUNTIME_SIM_NODE_H
#define AYNI_RUNTIME_SIM_NODE_H
#include <ayni/ayni.h>
#include <ayni/runtime/common/clock.h>
#include <memory>
namespace ayni::runtime {
class SimNode {
  public:
    SimNode();
    ~SimNode();
    SimNode(const SimNode &) = delete;
    SimNode &operator=(const SimNode &) = delete;
    // Creates CONFIGURING; registration and start belong to the caller.
    ayni_status_t assemble(const ayni_config_v1 &config);
    // Completes Host requests and advances fake time; no wall-clock sleeping.
    void advance(uint64_t microseconds);
    void set_utc(int64_t seconds, ayni_utc_trust_t trust = AYNI_UTC_TRUSTED);
    // Only CONFIGURING/STOPPED; successful release preserves Host storage.
    ayni_status_t release();
    ayni_kernel_t *kernel() const;
    Clock clock() const; // Fake clock ports, borrowed until release.

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::runtime
#endif
