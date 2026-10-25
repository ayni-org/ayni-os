// ACTION_STATE v1 body, contracts/storage/records.md. No native struct bytes
// or volatile submit claims cross this boundary; the outer record is REQUIRED.
#ifndef AYNI_CORE_WORK_ACTION_ACTION_RECORDS_H
#define AYNI_CORE_WORK_ACTION_ACTION_RECORDS_H

#include "durability/batch/record_body.h"
#include "durability/batch/record_types.h"
#include "work/action/action_record.h"

namespace ayni::work {
constexpr uint16_t action_state_record_type = durability::record_type::action_state;
constexpr uint16_t action_state_record_version = 1;
// Two identity u64, four scalar u32, two three-u64 stamps, one string length.
constexpr uint32_t action_state_record_fixed_size = 84;
constexpr uint32_t action_state_record_max_size =
    action_state_record_fixed_size + catalog::CatalogId::max_size;

// Shared three-u64 durable stamp layout, used by Action and enclosing Call records.
void write_action_stamp(ActionStamp stamp, durability::BodyWriter &writer);
ActionStamp read_action_stamp(durability::BodyReader &reader);

// Requires a valid fact; the only variable field is the exact binding ID.
uint32_t action_state_record_size(const ActionFact &fact);
// Writes exactly record_size(fact) bytes after owner admission; no revalidation.
void write_action_state_record(const ActionFact &fact, uint8_t *body);
// Parse locally, then share ActionRecord's shape validator. Failure preserves
// out; success does not prove cross-record relations or historical durability.
bool read_action_state_record(const uint8_t *body, uint32_t size, ActionFact &out);
} // namespace ayni::work
#endif
