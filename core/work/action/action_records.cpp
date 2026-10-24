#include "work/action/action_records.h"

#include "durability/batch/record_body.h"

namespace ayni::work {
namespace {
// Both preparation and latest transition use this one persistent stamp layout.
void write_stamp(ActionStamp stamp, durability::BodyWriter &writer) {
    writer.u64(stamp.boot_epoch);
    writer.u64(stamp.transition.transition_id);
    writer.u64(stamp.transition.commit_seq);
}

ActionStamp read_stamp(durability::BodyReader &reader) {
    return {reader.u64(), {reader.u64(), reader.u64()}};
}
} // namespace

uint32_t action_state_record_size(const ActionFact &fact) {
    return action_state_record_fixed_size + fact.identity.binding_id.size();
}

void write_action_state_record(const ActionFact &fact, uint8_t *body) {
    durability::BodyWriter writer(body, action_state_record_size(fact));
    writer.u64(fact.identity.operation_id);
    writer.u64(fact.identity.logical_call_id);
    writer.u32(fact.identity.binding_version);
    writer.u32(static_cast<uint32_t>(fact.phase));
    writer.u32(static_cast<uint32_t>(fact.effect));
    writer.u32(static_cast<uint32_t>(fact.evidence));
    write_stamp(fact.prepared, writer);
    write_stamp(fact.latest, writer);
    writer.bytes(fact.identity.binding_id.data(), fact.identity.binding_id.size());
}

// The string is first admitted by CatalogId, then fact validation checks its
// presence and its relationships. A later bad field never publishes a prefix.
bool read_action_state_record(const uint8_t *body, uint32_t size, ActionFact &out) {
    if (body == nullptr || size < action_state_record_fixed_size ||
        size > action_state_record_max_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    ActionFact fact{};
    fact.identity.operation_id = reader.u64();
    fact.identity.logical_call_id = reader.u64();
    fact.identity.binding_version = reader.u32();
    fact.phase = static_cast<ActionPhase>(reader.u32());
    fact.effect = static_cast<ActionEffect>(reader.u32());
    fact.evidence = static_cast<ActionEvidence>(reader.u32());
    fact.prepared = read_stamp(reader);
    fact.latest = read_stamp(reader);
    uint32_t id_size = 0;
    const auto *id = reader.bytes(catalog::CatalogId::max_size, id_size);
    if (!fact.identity.binding_id.assign({reinterpret_cast<const char *>(id), id_size}) ||
        !reader.ok() || !ActionRecord::valid(fact)) {
        return false;
    }
    out = fact;
    return true;
}
} // namespace ayni::work
