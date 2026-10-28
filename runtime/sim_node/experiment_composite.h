// E4 composition root. Application owns ordering; all persistent work and
// authority belong to Core. Controls below operate synchronous simulated ports.
#ifndef AYNI_EXPERIMENT_COMPOSITE_H
#define AYNI_EXPERIMENT_COMPOSITE_H
#include "experiment_support.h"
#include "fixture_model.h"
#include <ayni/memory.h>
#include <ayni/providers/mock/subscription.h>
namespace ayni::runtime::experiments {
enum class CompositeScenario {
    register_and_run,
    mismatched_binding_rejected,
    typed_schema_rejected,
    parent_cancel_propagates,
    unknown_step_blocks_dependents,
    round_over_eight_rejected,
    delete_during_analysis,
    revoke_during_analysis,
    subscription_closed_old_generation,
    v2_only_pauses_old_operation,
    resolver_reconciles_v1,
    unknown_goal_type_rejected,
    unsupported_format_preserved,
    unsupported_legacy_schema,
    unsupported_record_version,
    goal_v2_holds_old_goal,
    scene_v2_blocks_old_permit,
    rollback_unreadable_state,
    compat_matrix_matches,
    queued_not_presented_restart,
    present_success_commits_with_task,
    present_unknown_pending,
    present_failed_pending
};
enum class VersionFault { schema, required_record };
enum class CompositeCut {
    action_receipt,
    capture_prepare,
    capture_dispatch,
    analysis_prepare,
    analysis_dispatch,
    analysis_receipt,
    summary,
    capture_stop,
    presentation_prepare,
    presentation_receipt,
    task_complete
};
class Composite {
  public:
    explicit Composite(World &world) : world_(world) {}
    bool register_profile(ayni_kernel_t *);
    bool advance();
    void abandon_kernel();
    void begin_close();
    ayni_status_t close();
    bool run(CompositeScenario);
    bool run_cut(CompositeCut, hosts::sim::CutAppend);
    uint64_t completed() const { return completed_ ? 1 : 0; }

  private:
    static constexpr uint64_t model_delay_us = 1000;
    static constexpr uint32_t future_version = 2;
    World &world_;
    fixtures::ModelProbe model_;
    fixtures::ModelProbe tools_model_;
    providers::mock::SubscriptionProvider camera_;
    std::optional<ayni_provider_v1> model_offer_, camera_offer_, tools_offer_;
    std::optional<ayni_subscription_open_v1> opening_;
    std::array<ayni_call_resource_v1, 1> notification_resource_{
        {{text("notification.slot"), 1, 0}}};
    ActionService notification_{"notify.present", notification_resource_.data(), 1,
                                AYNI_CALL_EFFECT_NOTIFICATION};
    bool model_delivery_ = false, camera_delivery_ = true, closing_ = false;
    bool completed_ = false;
    uint32_t strategy_version_ = 1, goal_version_ = 1;
    uint64_t barriers_ = 0, parent_ = 0, goal_ = 0, source_ = 0;
    uint64_t source_owner_ = 1; // Agent number for fixture source; runtime actor remains Agent 1.
    std::optional<uint64_t> share_;
    static ayni_submit_result_t camera_submit(void *, const ayni_provider_request_v1 *,
                                              ayni_provider_completion_t *);
    enum class Preparation { live_action, prepared_action };
    bool prepare(Preparation = Preparation::live_action);
    bool advance_action();
    bool analysis_pending();
    bool presentation_pending();
    bool finish();
    uint64_t create(uint64_t source, uint64_t agent = 1);
    uint64_t save_source(uint64_t agent = 1);
    ayni_task_info_v1 task() const;
    ayni_call_info_v1 call(const char *step) const;
    ayni_subscription_info_v1 subscription() const;
    uint32_t candidates() const;
    bool change(uint64_t goal, ayni_intent_kind_t);
    bool restart_boundary(CompositeCut, hosts::sim::CutAppend);
    bool cancel_parent();
    bool unknown_action();
    enum class SourceChange { erase, revoke };
    bool invalidate_source(SourceChange);
    bool close_generation();
    enum class VersionCase { incompatible, resolver };
    bool version_boundary(VersionCase);
    bool storage_boundary(VersionFault, uint16_t version);
    bool goal_version_boundary();
    bool scene_version_boundary();
    bool compatible_current_profile();
    bool presentation(CompositeScenario);
    bool registration_boundaries(CompositeScenario);
    bool oversized_round();
};
// Tests-only CRC-correct version injection uses the existing batch/record codecs.
// It changes only the simulated media while no Kernel or producer exists.
bool replace_storage_version(World &, VersionFault, uint16_t version);
} // namespace ayni::runtime::experiments
#endif
