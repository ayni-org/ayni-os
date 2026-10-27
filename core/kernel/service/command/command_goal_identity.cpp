#include "kernel/service/command/command_goal_identity.h"

#include "foundation/kernel_id.h"

#include <cstring>

namespace ayni::kernel {
namespace {
ayni_string_v1 read_id(durability::BodyReader &reader) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(AYNI_CATALOG_ID_MAX_BYTES, size);
    return {reinterpret_cast<const char *>(bytes), size};
}
bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size &&
           (left.size == 0U || std::memcmp(left.data, right.data, left.size) == 0);
}
} // namespace

bool valid_goal_creation_identity(const GoalCreate &create) {
    return foundation::is_kernel_id(create.owner_instance_id) && create.goal_type_version != 0U &&
           (create.parent_goal_id == 0U || foundation::is_kernel_id(create.parent_goal_id)) &&
           create.parameters.size <= AYNI_GOAL_PARAMETER_MAX_BYTES_MAX &&
           work::check_goal_form(create) == AYNI_OK;
}

void write_goal_creation_identity(const GoalCreate &create, durability::BodyWriter &writer) {
    writer.u64(create.owner_instance_id);
    writer.u64(create.parent_goal_id);
    writer.u32(create.goal_type_version);
    writer.bytes(create.goal_type_id.data, create.goal_type_id.size);
    writer.bytes(create.parameters.data, create.parameters.size);
    writer.u32(create.trigger.kind);
    writer.u64(create.trigger.delay_us);
    writer.u64(create.trigger.period_us);
    writer.u64(create.expiry_us);
    writer.bytes(create.trigger_source.data, create.trigger_source.size);
    if (create.calendar.kind != 0) {
        work::write_calendar_rule(create.calendar, writer);
    }
    if (create.pause_group != 0) {
        writer.u32(create.pause_group);
    }
}

GoalCreate read_goal_creation_identity(durability::BodyReader &reader) {
    GoalCreate create{};
    create.owner_instance_id = reader.u64();
    create.parent_goal_id = reader.u64();
    create.goal_type_version = reader.u32();
    create.goal_type_id = read_id(reader);
    uint32_t size = 0;
    const auto *parameters = reader.bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, size);
    create.parameters = {parameters, size};
    create.trigger.kind = reader.u32();
    create.trigger.delay_us = reader.u64();
    create.trigger.period_us = reader.u64();
    create.expiry_us = reader.u64();
    create.trigger_source = read_id(reader);
    if (work::is_calendar_trigger(create.trigger.kind)) {
        if (!work::read_calendar_rule(reader, create.calendar)) {
            create.calendar = {}; // Shared shape check refuses the missing required rule.
        }
    }
    if (reader.remaining() != 0) {
        create.pause_group = reader.u32();
    }
    return create;
}

bool matches_goal_creation(const GoalCreate &create, const work::Goal &goal,
                           ayni_bytes_v1 parameters, const work::CalendarRule *calendar) {
    return goal.pause_group == create.pause_group &&
           goal.owner_instance_id == create.owner_instance_id &&
           goal.parent_goal_id == create.parent_goal_id &&
           goal.goal_type_version == create.goal_type_version &&
           goal.goal_type_id.equals(create.goal_type_id) &&
           same_bytes(parameters, create.parameters) && goal.trigger.kind == create.trigger.kind &&
           goal.trigger.delay_us == create.trigger.delay_us &&
           goal.trigger.period_us == create.trigger.period_us &&
           goal.expiry_after_us == create.expiry_us &&
           goal.trigger_source.equals(create.trigger_source) &&
           (create.calendar.kind == 0 ||
            (calendar != nullptr && work::same_calendar_rule(create.calendar, *calendar)));
}
} // namespace ayni::kernel
