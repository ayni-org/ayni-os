// Synchronous owner-thread evaluator for registered ReflexRules (ADR-0112).
// Runs in ayni_step() right after draining lock-free ObservationSlots and before
// CallRuntime::refresh_live_gates() / apply_calls(), enabling same-step sensor-to-action reaction
// with zero LLM latency and zero WAL traffic while enforcing every Grant/Scene/PermitGate rule.
#ifndef AYNI_CORE_KERNEL_REFLEX_REFLEX_ENGINE_H
#define AYNI_CORE_KERNEL_REFLEX_REFLEX_ENGINE_H

#include "kernel/service/audit/audit_ring.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/model/work_model.h"
#include "kernel/service/reflex/reflex_table.h"
#include "knowledge/observation/observation_table.h"

#include <cstdint>

namespace ayni::kernel {

class ReflexEngine {
  public:
    // Borrowed owners outlive this evaluator; no arena allocation of its own.
    ReflexEngine(ReflexTable &table, const WorkModel &work,
                 const knowledge::ObservationTable &observations, CallRuntime &calls,
                 AuditRing &audit)
        : table_(table), work_(work), observations_(observations), calls_(calls), audit_(audit) {}
    ReflexEngine(const ReflexEngine &) = delete;
    ReflexEngine &operator=(const ReflexEngine &) = delete;

    // Resumes a priority-ordered pass up to one matched action or the step cutoff.
    // A matched action consumes one item; inspection of disabled/idle rules consumes time only.
    enum class Advance { idle, applied, timed_out, internal };
    Advance advance(OwnerClock clock, uint64_t cutoff_us, uint64_t durable_seq);
    // Owner-side enable may change a rule already passed by the cursor.
    void request_pass() {
        next_rule_ = 0;
        pass_pending_ = true;
    }
    bool pending() const { return pass_pending_; }
    void reset_pass() {
        next_rule_ = 0;
        pass_pending_ = false;
    }
    // Clears active_call_id on any PREEMPT_CALL rule whose spawned EPHEMERAL call finished.
    void reap_completed();
    // Earliest staleness deadline for enabled STALE_OR_GAP fail-safe rules whose resource
    // currently holds an active EPHEMERAL Call; UINT64_MAX when none is pending.
    uint64_t next_deadline_us(uint64_t now_us, uint64_t durable_seq) const;

  private:
    bool scene_active(const ReflexRuleEntry &rule, uint64_t now_us, uint64_t durable_seq) const;
    bool evaluate_trigger(const ReflexRuleEntry &rule, uint64_t now_us,
                          ayni_catalog_name_v1 &out_source, int64_t &out_value) const;
    ayni_status_t execute_rule(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                               const ayni_catalog_name_v1 &source, int64_t value);

    uint32_t next_rule_ = 0;    // Next rule in the current priority pass, zero at pass start.
    bool pass_pending_ = false; // An interrupted pass still has rules to visit.
    ReflexTable &table_;        // Sole owner of rules and event ring.
    const WorkModel &work_;     // Read-only Scene/Grant/Agent owner.
    const knowledge::ObservationTable &observations_; // Latest owner-thread observations.
    CallRuntime &calls_;                              // Resource preemption and reflex Call owner.
    AuditRing &audit_;                                // Kernel-owned RAM audit ring (ADR-0113).
};

} // namespace ayni::kernel
#endif
