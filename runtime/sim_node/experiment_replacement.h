#include <ayni/workspace.hpp>
// E3 composition and evidence. Policies own decisions, Providers own deliveries,
// and Core owns every durable identity and checkpoint.
#ifndef AYNI_EXPERIMENT_REPLACEMENT_H
#define AYNI_EXPERIMENT_REPLACEMENT_H
#include "experiment_support.h"
#include "fixture_model.h"
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/memory.h>
#include <ayni/runtime/common/memory_bodies.h>
#include <ayni/strategies/builtin.h>
namespace ayni::runtime::experiments {
enum class ReplacementScenario {
    baseline_checkpoint,
    swap_provider_late_old_result,
    unknown_call_not_rebound,
    new_call_uses_new_binding,
    completed_operation_returns_old,
    swap_strategy_at_checkpoint,
    incompatible_private_checkpoint,
    unknown_strategy_version
};
class Replacement {
  public:
    explicit Replacement(World &world) : world_(world) {}
    bool register_profile(ayni_kernel_t *);
    bool advance();
    void abandon_kernel();
    void begin_close() { deliver_ = true; }
    ayni_status_t close();
    bool run(ReplacementScenario);
    uint64_t completed() const { return completed_ ? 1 : 0; }

  private:
    enum class Policy { rules, tool_loop, incompatible, unknown_version };
    World &world_;
    ayni::Workspace loop_workspace_, conversation_workspace_; // Distinct owner/validator regions.
    runtime::MemoryBodies bodies_; // Conversation texts and model bodies for the simulated run.
    strategies::ToolLoop loop_;
    bool configured_ = false; // Immutable concrete strategy configuration installed once.
    static ayni_status_t loop(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
    fixtures::ModelProbe a_, b_;
    struct Offers {
        ayni_provider_v1 a, b;
    };
    std::optional<Offers> offers_;
    providers::mock::Variant provider_ = providers::mock::Variant::a;
    Policy selected_ = Policy::rules;
    bool deliver_ = false; // External fake transport gate; never policy input.
    bool completed_ = false;
    uint64_t barriers_ = 0;
    uint64_t workflow_ = 0, pending_goal_ = 0, memory_ = 0;
    struct Baseline {
        ayni_task_info_v1 task, pending;
        ayni_call_info_v1 action, model;
        ayni_memory_info_v1 memory;
        std::vector<ayni_grant_info_v1> grants;
        uint64_t instance_a, instance_b, boot;
        TaskProgressEvidence progress;
        CallRouteEvidence model_route;
    };
    std::optional<Baseline> baseline_;
    uint64_t create(const char *type);
    ayni_task_info_v1 task(uint64_t goal) const;
    ayni_call_info_v1 call(uint64_t id) const;
    ayni_call_info_v1 action(uint64_t task) const;
    ayni_memory_info_v1 memory() const;
    bool change_goal(uint64_t goal, ayni_intent_kind_t);
    bool prepare();
    bool remember();
    bool preserved();
    bool restart(providers::mock::Variant, Policy);
    bool complete_workflow();
    bool provider_swap();
    bool compatible_swap();
    bool incompatible(Policy);
};
} // namespace ayni::runtime::experiments
#endif
