#include <ayni/workspace.hpp>
// Full shared-service experiments assemble public reusable components. Scenario
// assertions are external observations; Core remains the owner of all work facts.
#ifndef AYNI_EXPERIMENT_SHARED_H
#define AYNI_EXPERIMENT_SHARED_H
#include "experiment_support.h"
#include "fixture_model.h"
#include <ayni/applications/assistant.h>
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/applications/normal_fixtures.h>
#include <ayni/memory.h>
#include <ayni/providers/mock/subscription.h>
#include <ayni/resources.h>
#include <ayni/runtime/common/memory_bodies.h>
#include <ayni/strategies/builtin.h>
#include <optional>
namespace ayni::runtime::experiments {
class World;
enum class SharedScenario {
    model_slot_contention,
    local_rule_progresses,
    private_read_denied,
    explicit_share_then_revoke,
    profile_scoped_to_agent,
    voice_cannot_grant,
    pause_keeps_reminder,
    presentation_success_completes,
    presentation_failed_pending,
    presentation_unknown_pending,
    duplicate_receipt_once,
    receipt_lost_reconciled,
    quiet_hours_deferred,
    estop_keeps_reminder,
    completion_slots_when_queue_full,
    ordinary_work_expires_with_reason,
    model_limit_two,
    exclusive_queue_shared_parallel,
    resource_group_all_or_nothing,
    timeout_cancel_releases,
    close_stops_generation,
    renewal_rechecks_grant,
    delete_blocks_stale_summary,
    user_edit_not_overwritten,
    cleanup_resumes_after_restart,
    revoke_blocks_stale_summary,
    restart_keeps_model_calls
};
class SharedServices {
  public:
    explicit SharedServices(World &world) : world_(world) {}
    bool register_profile(ayni_kernel_t *kernel);
    bool advance();
    void abandon_kernel();
    ayni_status_t close();
    const ayni_body_store_v1 *bodies() const { return bodies_.store(); }
    bool begin_close();
    uint64_t clock_delta(const ayni_step_report_v1 &last, uint64_t seen_wakes) const;
    bool run(SharedScenario scenario);
    uint64_t completed() const;

  private:
    static constexpr uint64_t model_delay_us = 100000;
    static constexpr uint32_t model_timeout_ms = 1000;
    static constexpr uint32_t response_step_bound = 10;
    World &world_;
    fixtures::ModelProbe model_a_, model_b_;
    applications::normal_fixtures::Policy policy_;
    ayni::Workspace loop_workspace_, conversation_workspace_; // Distinct owner/validator regions.
    runtime::MemoryBodies bodies_; // Conversation texts and model bodies for the simulated run.
    strategies::ToolLoop tool_loop_;
    applications::experiment_fixtures::Services services_policy_;
    providers::mock::SubscriptionProvider camera_;
    std::optional<ayni_provider_v1> camera_offer_;
    std::optional<ayni_subscription_open_v1>
        last_opening_; // Copied value; sink stays borrowed only while Kernel lives.
    std::array<ayni_call_resource_v1, 2> group_resources_{
        {{text("microphone.slot"), 1, 0}, {text("test.actuator"), 1, 0}}};
    ActionService microphone_{"test.microphone", group_resources_.data(), 1};
    ActionService group_{"test.group", group_resources_.data(),
                         static_cast<uint32_t>(group_resources_.size())};
    std::optional<applications::assistant::Goals> assistant_;
    std::array<ayni_call_resource_v1, 1> notification_resources_{
        {{text("notification.window"), 1, 0}}};
    ActionService notification_{"notify.present", notification_resources_.data(), 1,
                                AYNI_CALL_EFFECT_NOTIFICATION};
    applications::assistant::QuietHours quiet_;
    bool update_presentation_window();
    enum class PresentationCase { success, failed, unknown, duplicate, lost, quiet, pause, estop };
    uint64_t reminder();
    bool present(PresentationCase);
    bool register_assistant(ayni_kernel_t *);
    uint64_t organize(uint64_t source, uint64_t agent = 1);
    enum class SourceChange { erase, edit, revoke, restart_cleanup };
    bool invalidate_summary(SourceChange);
    bool completion_pressure();
    bool ordinary_expiry();
    bool register_resources(ayni_kernel_t *);
    uint64_t create_service(const char *type, uint64_t agent);
    ayni_resource_info_v1 resource(const char *id) const;
    ayni_subscription_info_v1 subscription(uint64_t goal) const;
    bool cancel(uint64_t goal);
    bool change_goal(uint64_t goal, ayni_intent_kind_t kind);
    bool resources_parallel();
    bool resource_group();
    bool timeout_cancel();
    bool subscription_close();
    bool subscription_revoke();
    bool open_camera(uint64_t &goal, uint64_t agent = 1);
    static ayni_submit_result_t camera_submit(void *, const ayni_provider_request_v1 *,
                                              ayni_provider_completion_t *);
    struct Offers {
        ayni_provider_v1 a, b;
    };
    std::optional<Offers> offers_; // Views owned by configured model adapters until close.
    uint64_t barrier_checks_ = 0;
    uint64_t camera_renewals_ = 0; // Accepted local lease controls; external adapter diagnostic.
    enum class RunState { idle, running, completed } state_ = RunState::idle;
    uint64_t create_chat(uint64_t agent);
    uint64_t create(applications::normal_fixtures::GoalKind, uint64_t agent,
                    ayni_bytes_v1 parameters = {});
    ayni_task_info_v1 task(uint64_t goal) const;
    ayni_call_info_v1 call(uint64_t goal) const;
    ayni_resource_info_v1 model_slot() const;
    bool model_competition();
    bool local_rule();
    bool contention(uint64_t &agent_a_goal, uint64_t &agent_b_goal);
    bool privacy_denial();
    bool share_then_revoke();
    bool profile_scope();
    bool voice_boundary();
    uint64_t exhaust_model_task();
    bool model_limit();
    bool restart_limit();
    uint64_t save_memory(uint64_t agent, const char *body,
                         ayni_memory_namespace_t kind = AYNI_MEMORY_PRIVATE);
    uint64_t read_memory(uint64_t memory, uint64_t agent);
    uint64_t share_memory(uint64_t memory, uint64_t agent);
    bool read_outcome(uint64_t goal, ayni_status_t expected);
    bool trace_private_bodies_absent() const;
    bool finish_models(uint64_t agent_a_goal, uint64_t agent_b_goal);
};
} // namespace ayni::runtime::experiments
#endif
