// Durable physical execution/session constraints and independent local stop facts.
#ifndef AYNI_CORE_WORK_EXECUTION_FACT_H
#define AYNI_CORE_WORK_EXECUTION_FACT_H
#include "catalog/execution/execution_registry.h"
#include "foundation/record_body.h"
#include "foundation/transition_stamp.h"
#include <optional>
namespace ayni::work {
struct ExecutionFact {
    catalog::ExecutionBinding binding; // Exact original executor/Scene/parameter mapping.
    uint64_t
        scene_seq; // Positive explicit enable generation; disable/re-enable invalidates old work.
    uint64_t control_domain;      // Positive registered executor domain, not a resource ordinal.
    uint32_t control_mode;        // Positive original executor control mode.
    ayni_permit_epochs_v1 epochs; // Original complete generations; never regenerated during replay.
    ayni_execution_phase_t phase =
        AYNI_EXECUTION_PREPARED; // Permit possibility vs independent stopped proof.
    ayni_execution_target_t target =
        AYNI_TARGET_UNVERIFIED; // Measured separately from Provider success.
    ayni_control_stop_cause_t stop_cause =
        AYNI_CONTROL_STOP_NONE; // Original local stop measurement reason.
    uint32_t dependency_count;  // Valid reference prefix, 0..OBSERVATION_CONDITION_COUNT_MAX.
    std::array<ayni_observation_ref_v1, AYNI_OBSERVATION_CONDITION_COUNT_MAX>
        dependencies; // Exact current-boot evidence.
};
// Intrinsic execution/stop facts remain checkable after private request content is erased.
bool valid_execution_metadata(const ExecutionFact &fact);
// Core declaration/request validation, also used for original persisted facts.
bool valid_execution(const ExecutionFact &fact, ayni_bytes_v1 arguments);
bool same_execution_identity(const ExecutionFact &left, const ExecutionFact &right);
bool execution_advances(const std::optional<ExecutionFact> &before,
                        const std::optional<ExecutionFact> &after);
bool execution_stopped(const ExecutionFact &fact); // STOPPED or proven NOT_STARTED only.
// One intrinsic stopped-phase/cause/target rule for live confirmation and retained history.
bool valid_execution_stop(ayni_execution_phase_t phase, ayni_control_stop_cause_t cause,
                          ayni_execution_target_t target);
// Current CALL_STATE execution tail; no native struct bytes or monotonic times.
constexpr uint32_t execution_record_fixed_bytes = 112;
constexpr uint32_t execution_field_fixed_bytes = 16;
// Every reference includes subscription_id, zero explicitly denotes a Host source.
constexpr uint32_t execution_dependency_fixed_bytes = 40;
uint32_t execution_record_max();
uint32_t execution_record_size(const ExecutionFact &fact);
void write_execution(const ExecutionFact &fact, foundation::BodyWriter &writer);
bool read_execution(foundation::BodyReader &reader, ExecutionFact &out);
} // namespace ayni::work
#endif
