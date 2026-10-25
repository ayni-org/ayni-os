// CALL_STATE v1, contracts/storage/records.md. A full row keeps Task progress,
// original input/contract and the embedded action fact in one atomic batch.
#ifndef AYNI_CORE_WORK_CALL_CALL_RECORDS_H
#define AYNI_CORE_WORK_CALL_CALL_RECORDS_H

#include "work/call/call_table.h"

namespace ayni::work {
constexpr uint16_t call_record_type = 21; // REQUIRED; older readers explicitly refuse calls.
constexpr uint16_t call_record_version = 1;
constexpr uint32_t call_record_fixed_size = 288; // Fixed fields and ten string/body lengths.
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
