// Deterministic single-owner composition root. The in-memory Host survives a
// clean Kernel release/reassemble, so fixtures can continue on the same storage.
#ifndef AYNI_RUNTIME_SIM_NODE_H
#define AYNI_RUNTIME_SIM_NODE_H
#include <ayni/ayni.h>
#include <ayni/hosts/sim.h>
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
    // Owner-thread fault control; borrowed for this node's lifetime after assemble.
    // Exposes the simulated Host, never a production storage implementation.
    hosts::sim::Host *simulation();
    const hosts::sim::Host *simulation() const;
    // Simulates abrupt loss: the caller must first quiesce every Provider and
    // executor, dropping completion pointers without posting to the old Kernel.
    // Host requests end without receipts, then the old arena is discarded.
    // Preserves fake time and durable storage; assemble creates a new Kernel.
    ayni_status_t power_cut(hosts::sim::CutAppend append);
    ayni_kernel_t *kernel() const;
    Clock clock() const; // Fake clock ports, borrowed until release.

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::runtime
#endif
