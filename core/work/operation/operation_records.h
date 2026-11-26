// Schema12 unresolved ledger codecs (contracts/storage/records.md type29/30 v1).
#ifndef AYNI_CORE_WORK_OPERATION_OPERATION_RECORDS_H
#define AYNI_CORE_WORK_OPERATION_OPERATION_RECORDS_H
#include "durability/batch/record_types.h"
#include "work/operation/operation_ledger.h"
namespace ayni::work {
constexpr uint16_t unresolved_op_record_type = durability::record_type::unresolved_op;
constexpr uint16_t unresolved_op_state_record_type = durability::record_type::unresolved_op_state;
constexpr uint16_t operation_record_version = 1; // Snapshot full row and journal delta version.
constexpr uint32_t operation_record_fixed_bytes = 240; // Scalar header plus length/option markers.
constexpr uint32_t operation_query_bytes = 60; // Generation, phase, stamp, attempt, three scalars.
constexpr uint32_t operation_stop_bytes = 12;  // Phase, stop cause, target; no permit or lease.
constexpr uint32_t operation_subscription_bytes = 12; // Source length, version and lease duration.
constexpr uint32_t operation_state_bytes =
    84; // Actual fixed fields, including one canonical zero reserved word.
uint32_t operation_record_max(OperationLedger::Limits limits); // Bounded snapshot buffer need.
uint32_t operation_record_size(const OperationRecord &record); // Exact full row size, no envelope.
void write_operation_record(const OperationRecord &record, uint8_t *body); // Validated input only.
bool read_operation_record(foundation::Workspace &workspace, const uint8_t *body, uint32_t size,
                           OperationRecord &out); // No mutation.
void write_operation_change(const OperationChange &change,
                            uint8_t *body); // Exactly operation_state_bytes.
bool read_operation_change(const uint8_t *body, uint32_t size,
                           OperationChange &out); // Strict shape.
} // namespace ayni::work
#endif
