// Synchronous owner-thread evaluator for registered ReflexRules (ADR-0112).
// Runs in ayni_step() right after draining lock-free ObservationSlots and before
// CallRuntime::refresh_live_gates() / apply_calls(), enabling same-step sensor-to-action reaction
// with zero LLM latency and zero WAL traffic while enforcing every Grant/Scene/PermitGate rule.
#ifndef AYNI_CORE_KERNEL_REFLEX_REFLEX_ENGINE_H
#define AYNI_CORE_KERNEL_REFLEX_REFLEX_ENGINE_H

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
                 const knowledge::ObservationTable &observations, CallRuntime &calls)
        : table_(table), work_(work), observations_(observations), calls_(calls) {}
    ReflexEngine(const ReflexEngine &) = delete;
    ReflexEngine &operator=(const ReflexEngine &) = delete;

    // Evaluates all enabled ReflexRules in descending priority order.
    // Returns AYNI_OK normally or AYNI_INTERNAL on a Core invariant defect.
    ayni_status_t evaluate(uint64_t now_us, uint64_t durable_seq);
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

    ReflexTable &table_;                              // Sole owner of rules and event ring.
    const WorkModel &work_;                           // Read-only Scene/Grant/Agent owner.
    const knowledge::ObservationTable &observations_; // Latest owner-thread observations.
    CallRuntime &calls_;                              // Resource preemption and reflex Call owner.
};

} // namespace ayni::kernel
#endif
