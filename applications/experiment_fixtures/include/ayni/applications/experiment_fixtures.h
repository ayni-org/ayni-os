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
} // namespace ayni::applications::experiment_fixtures
#endif
