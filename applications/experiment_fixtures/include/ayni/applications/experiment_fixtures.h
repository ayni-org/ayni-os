// Finite service-contract workflows for the full deterministic experiments.
#ifndef AYNI_APPLICATIONS_EXPERIMENT_FIXTURES_H
#define AYNI_APPLICATIONS_EXPERIMENT_FIXTURES_H
#include <ayni/calls.h>
namespace ayni::applications::experiment_fixtures {
// Per-Task progress lives in Core; this policy owns immutable registration only.
class Services {
  public:
    ayni_status_t register_all(ayni_kernel_t *);

  private:
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
};
// Finite E3 workflow semantics; runtime selects and composes implementations.
namespace replacement {
ayni_strategy_v1 rule_strategy();
ayni_status_t register_goals(ayni_kernel_t *, ayni_string_v1 strategy_id,
                             ayni_status_t (*validate)(void *, ayni_bytes_v1),
                             void *validation_context);
// Returns handled=true when the shared prerequisite emitted a decision.
ayni_status_t prerequisite(const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &, bool &handled);
} // namespace replacement
// E4 application owns ordered steps; runtime owns concrete services and faults.
namespace composite {
ayni_status_t register_workflow(ayni_kernel_t *, uint32_t strategy_version = 1,
                                uint32_t goal_version = 1);
} // namespace composite
} // namespace ayni::applications::experiment_fixtures
#endif
