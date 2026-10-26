// CALL_STATE v1, contracts/storage/records.md. A full row keeps Task progress,
// original input/contract and the embedded action fact in one atomic batch.
#ifndef AYNI_CORE_WORK_CALL_CALL_RECORDS_H
#define AYNI_CORE_WORK_CALL_CALL_RECORDS_H

#include "durability/batch/record_types.h"
#include "work/call/call_table.h"

namespace ayni::work {
// REQUIRED; older readers explicitly refuse calls.
constexpr uint16_t call_record_type = durability::record_type::call;
constexpr uint16_t call_record_version = 1;
constexpr uint16_t call_execution_record_version =
    3; // Full original execution constraints and local stop facts.
constexpr uint16_t call_skipped_record_version = 2; // Same layout; adds Core-only SKIPPED outcome.
// Every row of a TOOL_CALLS contract: the v2 layout and outcomes plus a trailing
// result kind, so old readers explicitly refuse tool-call lists (ADR-0054).
constexpr uint16_t call_tool_calls_record_version = 4;
// Erased request/result bodies plus an explicit privacy stamp and execution-presence tag.
// Real action/query/execution outcomes retain their original encodings (ADR-0060).
constexpr uint16_t call_erased_record_version = 5;
constexpr uint32_t call_erasure_tail_size = 28; // u32 execution-present + 24-byte ActionStamp.
constexpr uint32_t call_result_kind_size = 4;   // The v4 trailing u32.
// Normal rows preserve v1; old readers explicitly refuse a background skip.
uint16_t call_record_version_for(const CallRecord &record);
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
bool read_call_record(const uint8_t *body, uint32_t size, CallRecord &out,
                      uint16_t version = call_record_version);
} // namespace ayni::work
#endif
