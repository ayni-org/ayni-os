// Full experiments assemble only simulated services. These controls are owner-thread
// fixture inputs, not production APIs or an alternative owner of Kernel facts.
#ifndef AYNI_RUNTIME_EXPERIMENT_SUPPORT_H
#define AYNI_RUNTIME_EXPERIMENT_SUPPORT_H
#include <array>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/runtime/sim_node.h>
#include <ayni/runtime/sim_node/experiments.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayni::runtime::experiments {
constexpr uint32_t operation_capacity = 16;
constexpr uint32_t step_limit = 2000;
constexpr uint32_t step_items = 64;
constexpr uint32_t step_time_us = 100000;
constexpr uint64_t tick_us = 1000;
enum class Metric {
    executions,
    pending_calls,
    pending_storage,
    goal_count,
    call_count,
    call_phase,
    call_outcome,
    call_effect,
    lifecycle,
    writer_epoch,
    faults_applied,
    submissions,
    call_durable,
    call_operation,
    task_count,
    call_query_conclusion,
    goal_state,
    task_state,
    kernel_boot,
    writer_advanced,
    boot_advanced,
    durable_kept,
    memory_count,
    task_steps,
    checkpoint_value,
    task_occurrence,
    model_calls_used,
    model_call_limit,
    grant_lifecycle,
    physical_submissions,
    physical_samples,
    physical_renewals,
    physical_challenges,
    held_renewals,
    control_phase,
    control_deadline,
    shared_completed,
    replacement_completed
};
ayni_string_v1 text(const char *value);
// Exact record boundaries are decoded only by the optional tests/ probe, using
// the existing storage owners' codecs; production runtime never interprets records.
enum class RecordPoint {
    goal_state,
    task,
    task_state,
    task_authority,
    prepared_call,
    dispatch_call,
    terminal_call,
    revoked_grant,
    memory,
    task_checkpoint,
    before_marker
};
enum class Damage { torn_tail, committed };

bool verify_completion_slots(
    class Evidence &); // Test-only owner probe; explicit UNSUPPORTED otherwise.
// Value-only inspection for experimental evidence absent from the public ABI.
// The optional tests/ implementation reads through the established test accessor.
struct TaskProgressEvidence {
    std::string writer;
    uint32_t writer_version = 0;
    std::vector<uint8_t> state;
    std::vector<std::string> steps;
    bool operator==(const TaskProgressEvidence &other) const {
        return writer == other.writer && writer_version == other.writer_version &&
               state == other.state && steps == other.steps;
    }
};
struct CallRouteEvidence {
    std::string binding, capability, provider, endpoint, step;
    uint32_t binding_version = 0, capability_version = 0, provider_version = 0;
    bool operator==(const CallRouteEvidence &other) const {
        return binding == other.binding && capability == other.capability &&
               provider == other.provider && endpoint == other.endpoint && step == other.step &&
               binding_version == other.binding_version &&
               capability_version == other.capability_version &&
               provider_version == other.provider_version;
    }
};
ayni_status_t inspect_task_progress(const ayni_kernel_t *, uint64_t task, TaskProgressEvidence &);
ayni_status_t inspect_call_route(const ayni_kernel_t *, uint64_t call, CallRouteEvidence &);

// One execution record belongs to the external test world, surviving Kernel loss.
// Execution counts every request to execute, so duplicate effects cannot be hidden
// by the ledger; this service deliberately does not perform operation-ID deduplication.
struct Effect {
    uint64_t operation = 0; // Zero is an unused slot; effects require a nonzero operation.
    uint32_t executions = 0;
};
class ActionService {
  public:
    // Descriptors live here until the entire deployment is quiescent.
    explicit ActionService(ayni_call_query_authority_t query = AYNI_CALL_QUERY_AUTHORITATIVE);
    ActionService(const char *capability, const ayni_call_resource_v1 *, uint32_t count,
                  uint32_t effects = 0);
    ayni_provider_v1 describe();
    ayni_call_capability_v1 capability() const { return capability_; }
    uint64_t executions() const;
    uint32_t pending() const;
    uint32_t pending_queries() const;
    uint64_t submissions() const { return submissions_; }
    // Explicit side effect and explicit receipt are separate cut points.
    ayni_status_t execute(ayni_kernel_t *kernel);
    ayni_status_t receipt(ayni_call_outcome_t outcome, ayni_call_effect_t effect);
    ayni_status_t answer_query(uint32_t conclusion);
    ayni_status_t repeat_receipt();
    ayni_status_t post_old_attempt();
    // All producers are synchronous owner controls: forget pointers before power loss.
    void quiesce();
    ayni_status_t drain(); // Synthetic service shutdown confirms its known effect, never hardware.

  private:
    struct Borrow {
        ayni_attempt_ref_v1 attempt{};
        uint64_t call = 0, operation = 0;
        ayni_provider_completion_t *completion = nullptr; // Null: this slot owns no borrow.
        bool executed = false; // External effect ledger for this delivery, not a Core state copy.
    };
    std::array<Borrow, operation_capacity> calls_{}, queries_{};
    std::array<Effect, operation_capacity> effects_{};
    ayni_call_capability_v1 capability_{};
    ayni_string_v1 provider_id_{}; // Static fixture identity, stable across simulated boots.
    uint64_t submissions_ = 0;     // Accepted deliveries, not effects; survives Kernel loss.
    struct LastReceipt {
        ayni_provider_completion_t *target;
        ayni_provider_result_v1 result;
    };
    std::optional<LastReceipt> last_receipt_;        // Only current arena; cleared by quiesce.
    std::optional<ayni_attempt_ref_v1> old_attempt_; // Value only, never an old completion pointer.
    static ayni_submit_result_t submit(void *, const ayni_provider_request_v1 *,
                                       ayni_provider_completion_t *);
    static ayni_submit_result_t query(void *, uint64_t, ayni_attempt_ref_v1,
                                      ayni_provider_completion_t *);
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1);
};

// Bounded recorder: keeps the first causal failure and deterministic scalar facts.
// Business bodies never enter this interface. Filesystem ownership is in the runner.
class Evidence {
  public:
    void fact(const char *category, const char *name, uint64_t value);
    void changed(const std::string &name, uint64_t value);
    void changed_status(const std::string &name, ayni_status_t value);
    void context(uint64_t step, uint64_t clock_us) {
        step_ = step;
        clock_us_ = clock_us;
    }
    bool status(const char *name, ayni_status_t actual, ayni_status_t expected = AYNI_OK);
    bool equal(const char *name, uint64_t actual, uint64_t expected);
    bool ok() const { return status_ == AYNI_OK; }
    const std::string &failure() const { return failure_; }
    std::string take_failure() { return std::move(failure_); }
    ayni_status_t error_status() const { return status_; }
    const std::vector<std::string> &lines() const { return lines_; }

  private:
    void failure(const std::string &message, ayni_status_t status = AYNI_INVALID_STATE);
    void record(const char *category, const char *name, const std::string &value);
    void changed_value(const std::string &name, const std::string &value);
    std::string failure_;
    ayni_status_t status_ = AYNI_OK;
    std::vector<std::string> lines_;
    std::map<std::string, std::string> snapshots_; // Diagnostic copies, bounded by profile objects.
    uint64_t step_ = 0, clock_us_ = 0; // Current observation point, in fake microseconds.
};

// Owns one complete simulated deployment. Registration/strategy may vary at restart;
// only the Host durable image and external service ledger survive an abrupt cut.
class Physical;
class SharedServices;
class Replacement;
class World {
  public:
    explicit World(Evidence &evidence,
                   ayni_call_query_authority_t query = AYNI_CALL_QUERY_AUTHORITATIVE,
                   Deployment deployment = Deployment::counter);
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    bool start(ayni_lifecycle_t expected = AYNI_LIFECYCLE_RUNNING);
    bool start_recovery();
    bool finish_recovery(ayni_lifecycle_t expected = AYNI_LIFECYCLE_RUNNING);
    bool recover();
    bool step();
    bool step_now(); // Run the same bounded Core step without advancing the shared clock.
    bool settle(uint32_t rounds = 32);
    bool crash(hosts::sim::CutAppend append);
    ayni_status_t close();
    bool until(const char *name, const std::function<bool()> &condition,
               uint32_t bound = step_limit);
    ayni_intent_result_v1 submit(ayni_intent_kind_t, const void *body, uint64_t command = 0,
                                 uint32_t revision = 0, ayni_status_t expected = AYNI_OK,
                                 std::optional<ayni_reason_t> reason = std::nullopt);
    uint64_t create(const char *type = "action", uint64_t command = 0, uint64_t agent = 1);
    bool grant(uint64_t agent = 1, const char *capability = "test.increment");
    bool request_query(ayni_status_t expected = AYNI_OK);
    bool save_call();
    bool same_call();
    bool saved_call_missing();
    bool retry(ayni_status_t expected = AYNI_OK);
    bool revoke();
    bool revoke(uint64_t grant_id);
    uint64_t owner_session() const { return session_; }
    uint64_t last_command() const { return next_command_; } // Diagnostic receipt lookup only.
    bool conflict_command();
    bool expire_command();
    bool read_result();
    bool cancel_goal();
    bool application_policy();
    bool exhaust_storage();
    void restore_storage();
    bool hold_record_append(RecordPoint point);
    bool crash_after_record(RecordPoint point);
    bool damage_log(Damage damage);
    bool fault_pending(hosts::sim::StorageFault fault);
    bool complete_storage();
    bool complete_old_writer();
    ayni_call_info_v1 call() const;
    ayni_kernel_info_v1 info() const;
    ayni_lifecycle_t lifecycle() const;
    uint32_t count(ayni_object_kind_t kind) const;
    uint64_t instance(uint64_t agent) const;
    uint64_t metric(Metric metric) const;
    // Current catalog inputs; they change only while no Kernel exists.
    ayni_config_v1 config;
    std::function<ayni_status_t(ayni_kernel_t *)> registration;
    std::function<ayni_status_t(const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &)> policy;
    SimNode node;
    ActionService service;
    Evidence &evidence;
    std::unique_ptr<Physical> physical;
    std::unique_ptr<SharedServices> shared;
    std::unique_ptr<Replacement> replacement;
    uint64_t steps = 0;
    uint32_t items_per_step = step_items; // Work items per bounded Core step, 1..step_items.
    enum class Storage { complete, held };
    Storage storage = Storage::complete;

  private:
    uint64_t session_ = 0, next_command_ = 0;
    // Test expectations only: snapshots never drive dispatch, authorization or recovery.
    struct SavedCall {
        ayni_call_info_v1 call;
        uint32_t writer_epoch;
        uint64_t kernel_boot, durable_seq;
    };
    std::optional<SavedCall> saved_call_;
    struct AcceptedGoal {
        std::string type; // Owns body.goal_type_id; other pointer fields are empty in this profile.
        ayni_goal_create_v1 body;
        uint64_t command;
        ayni_intent_result_v1 result;
    };
    std::optional<AcceptedGoal> accepted_goal_;
    std::optional<uint64_t> grant_;
    ayni_goal_create_v1 goal_body(const char *type, uint64_t agent) const;
    struct HostSchedule {
        ayni_step_report_v1 report;
        uint64_t seen_wakes; // Host wake count sampled before step; new wakes run immediately.
    };
    std::optional<HostSchedule>
        schedule_; // None before first step/restart; never a domain-state copy.
    ayni_status_t close_services();
    bool register_profile();
    bool observe();
    void detach_clients(); // Ends all synchronous client borrows before any arena loss.
    bool advance_step(uint64_t clock_delta_us);
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
};
} // namespace ayni::runtime::experiments
#endif
