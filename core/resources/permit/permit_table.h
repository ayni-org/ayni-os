// Boot-local permit ownership, polling and quarantine. No callbacks or work facts.
#ifndef AYNI_CORE_RESOURCES_PERMIT_TABLE_H
#define AYNI_CORE_RESOURCES_PERMIT_TABLE_H
#include "resources/admission/resource_admission.h"
#include <ayni/execution.h>
#include <optional>
namespace ayni::resources {
struct StopFact {
    ayni_execution_phase_t phase;    // STOPPED or authoritative NOT_STARTED only.
    ayni_control_stop_cause_t cause; // Measured local cause, NONE for NOT_STARTED.
    ayni_execution_target_t target;  // Independent measured target conclusion.
};
struct PermitLease {
    uint64_t call_id;        // Exact retained work identity, positive while present.
    uint32_t executor_index; // Stable catalog slot, also this table's slot.
    ResourceClaim claim;     // Exact acquisition capability; admission owns occupancy itself.
    ayni_execution_permit_v1 permit; // Copied issued value, including transient local nonce.
    uint64_t poll_after_us;          // Next Core-clock poll, finite; never persisted.
    enum class Phase {
        issued,
        stopping
    } phase = Phase::issued; // stopping has released into quarantine.
    enum class StopDelivery {
        pending,
        sent
    } stop_delivery = StopDelivery::pending; // Only used while stopping.
    ayni_status_t port_status =
        AYNI_OK; // Latest local control-port result; transient diagnostic, never stop proof.
    bool terminal_seen = false; // A Provider terminal is retained; not physical stop proof.
    std::optional<StopFact>
        stopped; // Exact authoritative measurement, retained until durable application.
};
class PermitTable {
  public:
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t executors);
    // One live local gate per executor; no table exists when executors == 0.
    PermitTable(void *storage, uint32_t executors, ResourceAdmission &resources);
    uint32_t capacity() const {
        return capacity_;
    } // Stable executor slots, independent of work replay.
    PermitLease *at(uint32_t index) {
        return slots_[index] ? &*slots_[index] : nullptr;
    } // Owner-thread lease borrow.
    const PermitLease *at(uint32_t index) const {
        return slots_[index] ? &*slots_[index] : nullptr;
    }
    bool available(uint32_t executor) const; // Pure preflight, no reservation or eviction.
    // first_poll_us is when the owner first reads the executor's status.
    bool install(uint64_t call_id, uint32_t executor, ResourceClaim claim,
                 const ayni_execution_permit_v1 &permit, uint64_t first_poll_us);
    PermitLease *find(uint64_t call_id); // Owner-thread borrow, null when no live lease.
    const PermitLease *find(uint64_t call_id) const;
    // First stop request quarantines actuator resources immediately, independently of log room.
    bool request_stop(uint64_t call_id);
    // Fresh trusted local fact; mismatched identity or malformed status cannot free resources.
    bool observe(uint64_t call_id, const ayni_executor_status_v1 &status);
    // Local absence is conclusive only after the Provider's exact terminal has ended its borrow.
    bool not_started(uint64_t call_id);
    // Caller has durably committed stopped, or is explicitly closing after the exact borrow
    // drains. Releases measured-stop quarantine; closing alone creates no durable success.
    bool release_stopped(uint64_t call_id);
    bool empty() const; // No borrowed executor/resource execution remains.
  private:
    using Slot = std::optional<PermitLease>;
    Slot *slots_;                  // Executor-indexed caller arena; no moving entries.
    uint32_t capacity_;            // Exact configured executor capacity.
    ResourceAdmission &resources_; // Sole physical occupancy/quarantine owner.
};
} // namespace ayni::resources
#endif
