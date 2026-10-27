// P6 normal fixtures (ADR-0068): compiled data and framework-independent execution.
#ifndef AYNI_RUNTIME_SIM_NODE_FIXTURES_H
#define AYNI_RUNTIME_SIM_NODE_FIXTURES_H
#include <ayni/applications/normal_fixtures.h>
#include <ayni/ayni.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace ayni::runtime::fixtures {
enum class Provider { a, b };
using Strategy = applications::normal_fixtures::PolicyRevision;
using Goal = applications::normal_fixtures::GoalKind;
enum class AssistantGoal { reminder, device_check };
enum class Metric {
    task_state,
    task_steps,
    model_calls,
    model_limit,
    hold_reason,
    hold_status,
    goal_group_paused,
    goal_state,
    goal_count,
    call_count,
    model_a_executions,
    model_b_executions,
    presentations,
    increments,
    health_executions,
    max_model_holders,
    max_camera_holders,
    local_steps,
    subscription_stops,
    barrier_checks,
    preserved_calls
};
enum class Compare { equal, at_most, at_least };
// All time fields are simulated microseconds. Capacities remain a fixed fixture profile.
struct Config {
    uint64_t tick_us;
    uint64_t model_delay_us;
    uint32_t step_units;
    uint32_t max_steps;
    uint32_t model_hard_limit;
};
// Labels resolve only within a case; no field can name a URL, file or external service.
struct Grant {
    const char *name;
    uint32_t agent;
    const char *capability;
};
struct SaveMemory {
    const char *name;
    uint32_t agent;
    const char *body;
};
struct ShareMemory {
    const char *name;
    const char *memory;
    uint32_t agent;
};
struct Revoke {
    const char *grant;
};
struct CreateGoal {
    const char *name;
    uint32_t agent;
    std::variant<Goal, AssistantGoal> kind;
    const char *memory;   // Empty except memory_read.
    uint32_t pause_group; // Zero means no group, as in the C ABI.
    int64_t calendar_s;   // UTC0 civil seconds for reminder; zero otherwise.
    const char *body;     // Synthetic reminder body; never included in traces.
};
struct Steps {
    uint32_t count;
};
struct Advance {
    uint64_t microseconds;
};
struct WallClock {
    int64_t seconds;
    ayni_utc_trust_t trust;
};
// Targets name Goals for Task metrics; empty for whole-fixture counters.
struct Expect {
    Metric metric;
    const char *target;
    Compare compare;
    int64_t value;
};
struct Wait {
    Expect condition;
};
struct Check {
    Expect condition;
}; // Assert now; do not advance fake time to make it true.
struct PauseGroup {
    uint32_t group;
};
struct ResumeGroup {
    uint32_t group;
};
struct PauseGoal {
    const char *goal;
};
struct ResumeGoal {
    const char *goal;
};
struct LocalInput {
    const char *goal;
};
struct Retry {
    const char *goal;
};
// A clean drain/reassemble only; neither action implements a fault or recovery campaign.
struct Reopen {
    Provider provider;
    Strategy strategy;
};
using Action = std::variant<Grant, SaveMemory, ShareMemory, Revoke, CreateGoal, Steps, Advance,
                            WallClock, Wait, Check, PauseGroup, ResumeGroup, PauseGoal, ResumeGoal,
                            LocalInput, Retry, Reopen>;
struct Case {
    const char *id;
    const char *experiment_step;
    const char *description;
    const char *clauses;
    const char *source; // Repository-relative JSON path.
    Config config;
    const Action *actions;
    uint32_t action_count;
    const Expect *expectations;
    uint32_t expectation_count;
};
struct Cases {
    const Case *data;
    size_t size;
};
Cases cases();                 // Generated immutable table, process lifetime.
const char *source_revision(); // Git revision and dirty marker when the table was generated.
struct Result {
    ayni_status_t status = AYNI_OK;
    std::string detail; // First failed action/assertion or classified runtime error; no bodies.
    ayni_status_t close_status =
        AYNI_OK; // Independent cleanup result; never replaces a prior cause.
    ayni_status_t artifact_status =
        AYNI_OK; // Independent output result; meaningful even if files are incomplete.
};
// Owner-thread native composition. data must be an element of cases().
// Creates output_directory exclusively, writes run.json /
// trace.jsonl, and closes all borrows before returning. Existing directories are refused.
Result run(const Case &, const char *output_directory);
} // namespace ayni::runtime::fixtures
#endif
