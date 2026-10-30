// CALL_STATE v1, contracts/storage/records.md. A full row keeps Task progress,
// original input/contract and the embedded action fact in one atomic batch.
#ifndef AYNI_CORE_WORK_CALL_CALL_RECORDS_H
#define AYNI_CORE_WORK_CALL_CALL_RECORDS_H

#include "durability/batch/record_types.h"
#include "work/call/call_table.h"

namespace ayni::work {
// One complete current row in journal and snapshot (schema 7).
constexpr uint16_t call_record_type = durability::record_type::call;
constexpr uint16_t call_record_version = 1;
constexpr uint32_t call_subscription_fixed_bytes = 44; // Attachment now belongs to common state.
// Execution/erasure markers, erasure stamp and result kind are always encoded.
constexpr uint32_t call_metadata_tail_size = 36;
// Original optional Grant ID/revision, contracts/storage/records.md.
constexpr uint32_t call_authorization_size = 12;
constexpr uint32_t call_mode_attachment_bytes = 8; // Two u32, contracts/storage/records.md.
constexpr uint32_t call_record_fixed_size = 292 + call_metadata_tail_size +
                                            call_authorization_size + call_mode_attachment_bytes +
                                            model_context_record_bytes + model_usage_record_bytes;
constexpr uint32_t call_record_id_count =
    6; // Step, binding, capability, provider, endpoint, destination.
constexpr uint32_t call_record_resource_fixed_size = 8; // String length and units per resource.
// Maximum full row body under caller-selected byte bounds.
uint32_t call_record_max(CallTable::Limits limits);
// Exact size and writer of an owner-validated row; excludes its outer record envelope.
uint32_t call_record_size(const CallRecord &record);
void write_call_record(const CallRecord &record, uint8_t *body);
// Parses once, then applies CallTable's shared intrinsic validator. Failure
// preserves out; success borrows domain, input and result from body.
bool read_call_record(const uint8_t *body, uint32_t size, CallRecord &out);
} // namespace ayni::work
#endif
