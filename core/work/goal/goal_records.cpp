#include "work/goal/goal_records.h"
#include "work/goal/calendar_table.h"

#include "durability/batch/record_body.h"

namespace ayni::work {
namespace {

constexpr uint32_t next_scheduled_flag = 0x1U;
constexpr uint32_t expiry_pending_flag = 0x2U;
constexpr uint32_t known_flags = next_scheduled_flag | expiry_pending_flag;

// Reads a catalog id field; empty only when may_be_empty. A read past the
// end shows in BodyReader::ok().
bool read_id(durability::BodyReader &reader, bool may_be_empty, catalog::CatalogId &out) {
    uint32_t size = 0;
    const uint8_t *data = reader.bytes(catalog::CatalogId::max_size, size);
    if (size == 0U) {
        return may_be_empty;
    }
    return out.assign({reinterpret_cast<const char *>(data), size});
}

} // namespace

// Schema 3 keeps all current creation fields; optional facts have explicit markers.
uint32_t goal_record_size(const Goal &goal) {
    return goal_record_fixed_size + goal.goal_type_id.size() + goal.trigger_source.size() +
           goal.parameter_size;
}

void write_goal_record(const Goal &goal, ayni_bytes_v1 parameters, uint8_t *body,
                       GoalRecordOptions options) {
    durability::BodyWriter writer(body, goal_record_size(goal));
    writer.u64(goal.goal_id);
    writer.u64(goal.parent_goal_id);
    writer.u64(goal.owner_instance_id);
    writer.u64(goal.replaced_goal_id);
    writer.u64(goal.origin.principal_id);
    writer.u32(goal.origin.kind);
    writer.u32(goal.origin.channel);
    writer.u32(goal.origin.binding_revision);
    writer.u32(goal.depth);
    writer.u32(goal.goal_type_version);
    writer.bytes(goal.goal_type_id.data(), goal.goal_type_id.size());
    writer.u32(goal.trigger.kind);
    writer.u64(goal.trigger.delay_us);
    writer.u64(goal.trigger.period_us);
    writer.bytes(goal.trigger_source.data(), goal.trigger_source.size());
    writer.u64(goal.expiry_after_us);
    writer.u64(goal.occurrence_wait_max_us);
    writer.bytes(parameters.data, parameters.size);
    writer.u32(goal.work_profile.kind);
    writer.u64(goal.work_profile.background_period_us);
    writer.u32(goal.pause_group);
    writer.u32(options.authority ? 1U : 0U);
    writer.u32(options.provenance ? 1U : 0U);
}

// Parse only the current layout. Partner markers replace old version-based promises.
bool read_goal_record(const uint8_t *body, uint32_t size, GoalRecord &out) {
    if (body == nullptr || size < goal_record_fixed_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    out = GoalRecord{};
    out.goal_id = reader.u64();
    out.goal.parent_goal_id = reader.u64();
    out.goal.owner_instance_id = reader.u64();
    out.replaced_goal_id = reader.u64();
    out.goal.origin.principal_id = reader.u64();
    out.goal.origin.kind = reader.u32();
    out.goal.origin.channel = reader.u32();
    out.goal.origin.binding_revision = reader.u32();
    out.depth = reader.u32();
    out.goal.goal_type_version = reader.u32();
    if (!read_id(reader, false, out.goal.goal_type_id)) {
        return false;
    }
    out.goal.trigger.kind = reader.u32();
    out.goal.trigger.delay_us = reader.u64();
    out.goal.trigger.period_us = reader.u64();
    const bool local = out.goal.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT;
    if (!read_id(reader, !local, out.goal.trigger_source) ||
        (!local && out.goal.trigger_source.size() != 0U)) {
        return false;
    }
    out.goal.expiry_us = reader.u64();
    out.goal.occurrence_wait_max_us = reader.u64();
    uint32_t parameter_size = 0;
    const uint8_t *parameters = reader.bytes(size, parameter_size);
    out.goal.parameters = {parameters, parameter_size};
    out.goal.work_profile.kind = reader.u32();
    out.goal.work_profile.background_period_us = reader.u64();
    out.goal.pause_group = reader.u32();
    if (!PauseGroups::valid_membership(out.goal.pause_group)) {
        return false;
    }
    const auto authority = reader.u32();
    const auto provenance = reader.u32();
    if (authority > 1 || provenance > authority) {
        return false;
    }
    out.authority_required = authority != 0;
    out.provenance_required = provenance != 0;
    // GoalTable owns trigger/profile validity for both parsed and typed inputs.
    return reader.ok();
}

void write_goal_state_record(const Goal &goal, uint8_t *body, GoalRecordChange change) {
    durability::BodyWriter writer(body, goal_state_record_size);
    writer.u64(goal.goal_id);
    writer.u64(goal.last_task_id);
    writer.u32(goal.lifecycle);
    writer.u32(goal.revision);
    writer.u32(goal.cancel_revision);
    writer.u32(goal.occurrences_skipped);
    writer.u32(goal.skip_reason);
    writer.u32(goal.cursor.taken);
    writer.u32(goal.cursor.waiting);
    writer.u32((!is_calendar_trigger(goal.trigger.kind) && goal.cursor.has_next()
                    ? next_scheduled_flag
                    : 0U) |
               (goal.expires_us != no_time ? expiry_pending_flag : 0U));
    writer.u32(static_cast<uint32_t>(change));
}

bool read_goal_state_record(const uint8_t *body, uint32_t size, GoalStateRecord &out) {
    if (body == nullptr || size != goal_state_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    out.goal_id = reader.u64();
    out.state.last_task_id = reader.u64();
    out.state.lifecycle = reader.u32();
    out.state.revision = reader.u32();
    out.state.cancel_revision = reader.u32();
    out.state.occurrences_skipped = reader.u32();
    out.state.skip_reason = reader.u32();
    out.state.taken = reader.u32();
    out.state.waiting = reader.u32();
    const uint32_t flags = reader.u32();
    out.state.next_scheduled = (flags & next_scheduled_flag) != 0U;
    out.state.expiry_pending = (flags & expiry_pending_flag) != 0U;
    const auto change = reader.u32();
    if (change < static_cast<uint32_t>(GoalRecordChange::state) ||
        change > static_cast<uint32_t>(GoalRecordChange::reauthorization)) {
        return false;
    }
    out.change = static_cast<GoalRecordChange>(change);
    return reader.ok() && (flags & ~known_flags) == 0U;
}

} // namespace ayni::work
