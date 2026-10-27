// Fixture-only policies exercise public contracts; no Kernel-private access.
#ifndef AYNI_APPLICATIONS_NORMAL_FIXTURES_H
#define AYNI_APPLICATIONS_NORMAL_FIXTURES_H
#include <array>
#include <ayni/calls.h>
namespace ayni::applications::normal_fixtures {
enum class PolicyRevision { v1, v2 };
enum class GoalKind {
    model,
    repeat_model,
    action,
    local,
    memory_read,
    checkpoint,
    composite,
    remote
};
inline constexpr uint32_t remote_destination_field = 1;
inline constexpr char remote_destination[] = "sim.node_b";
inline constexpr uint32_t goal_type_count = 8;
const char *goal_type(GoalKind);
// Owns immutable registration metadata; per-Task progress remains in Core's steps/results.
class Policy {
  public:
    Policy() = default;
    Policy(const Policy &) = delete;
    Policy &operator=(const Policy &) = delete;
    Policy(Policy &&) = delete;
    Policy &operator=(Policy &&) = delete;
    // Owner-thread CONFIGURING registration. Keep this metadata at its original
    // address until every borrowing Kernel has been successfully destroyed.
    ayni_status_t register_all(ayni_kernel_t *, PolicyRevision);

  private:
    std::array<GoalKind, goal_type_count> kinds_{
        {GoalKind::model, GoalKind::repeat_model, GoalKind::action, GoalKind::local,
         GoalKind::memory_read, GoalKind::checkpoint, GoalKind::composite, GoalKind::remote}};
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    static ayni_status_t validate_memory(void *, ayni_bytes_v1);
    static ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *);
};
} // namespace ayni::applications::normal_fixtures
#endif
