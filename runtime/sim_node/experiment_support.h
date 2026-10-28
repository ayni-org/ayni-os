// Full experiments assemble only simulated services. These controls are owner-thread
// fixture inputs, not production APIs or an alternative owner of Kernel facts.
#ifndef AYNI_RUNTIME_EXPERIMENT_SUPPORT_H
#define AYNI_RUNTIME_EXPERIMENT_SUPPORT_H
#include <array>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/runtime/sim_node.h>
#include <functional>
#include <map>
#include <string>
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
    faults_applied
};
ayni_string_v1 text(const char *value);

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
    ayni_provider_v1 describe();
    ayni_call_capability_v1 capability() const { return capability_; }
    uint64_t executions() const;
    uint32_t pending() const;
    uint32_t pending_queries() const;
    // Explicit side effect and explicit receipt are separate cut points.
    ayni_status_t execute(ayni_kernel_t *kernel);
    ayni_status_t receipt(ayni_call_outcome_t outcome, ayni_call_effect_t effect);
    ayni_status_t answer_query(uint32_t conclusion);
    // All producers are synchronous owner controls: forget pointers before power loss.
    void quiesce();

  private:
    struct Borrow {
        ayni_attempt_ref_v1 attempt{};
        uint64_t call = 0, operation = 0;
        ayni_provider_completion_t *completion = nullptr; // Null: this slot owns no borrow.
    };
    std::array<Borrow, operation_capacity> calls_{}, queries_{};
    std::array<Effect, operation_capacity> effects_{};
    ayni_call_capability_v1 capability_{};
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
    bool ok() const { return failure_.empty(); }
    const std::string &failure() const { return failure_; }
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
class World {
  public:
    explicit World(Evidence &evidence);
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    bool start();
    bool recover();
    bool step();
    bool settle(uint32_t rounds = 32);
    bool crash(hosts::sim::CutAppend append);
    ayni_status_t close();
    bool until(const char *name, const std::function<bool()> &condition,
               uint32_t bound = step_limit);
    ayni_intent_result_v1 submit(ayni_intent_kind_t, const void *body, uint64_t command = 0,
                                 uint32_t revision = 0, ayni_status_t expected = AYNI_OK);
    uint64_t create(const char *type = "action", uint64_t command = 0, uint64_t agent = 1);
    bool grant(uint64_t agent = 1);
    bool request_query();
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
    uint64_t steps = 0;
    enum class Storage { complete, held };
    Storage storage = Storage::complete;

  private:
    uint64_t session_ = 0, next_command_ = 0;
    bool register_profile();
    bool observe();
    static ayni_status_t decide(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *);
};
} // namespace ayni::runtime::experiments
#endif
