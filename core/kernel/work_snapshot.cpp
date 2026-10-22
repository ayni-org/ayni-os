#include "kernel/work_snapshot.h"

#include "authority/principal/owner_binding_records.h"
#include "durability/batch/record_body.h"
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"
#include "kernel/work_replay.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

// Every record a snapshot holds is at version 1 (records.md).
constexpr uint16_t snapshot_record_version = 1;
constexpr uint32_t record_prefix = static_cast<uint32_t>(durability::record_header_size);
constexpr uint32_t id_max = catalog::CatalogId::max_size;

// The tails after the log body (records.md). A stamp is a transition_id,
// then a commit_seq.
constexpr uint32_t stamp_commit_seq_at = 8;
constexpr uint32_t stamp_size = 16;
// OWNER_BINDING: the stamp that first established this owner.
constexpr uint32_t owner_binding_size = authority::owner_binding_record_size + stamp_size;
// INSTANCE: the stamp it last changed at.
constexpr uint32_t instance_size = work::instance_record_size + stamp_size;
// GOAL: the GOAL_STATE body, then its created_transition_id and the
// commit_seq it last changed at.
constexpr uint32_t goal_extra = work::goal_state_record_size + stamp_size;
// TASK: created_transition_id, the commit_seq it last changed at, steps_seq
// and checkpoint_seq, then steps_before.
constexpr uint32_t task_tail_size = 4U * 8U + 4U;
constexpr uint32_t task_size = work::task_record_size + task_tail_size;
// COMMAND_MAPPING: the commit_seq of the batch that accepted it.
constexpr uint32_t command_tail_size = 8;
constexpr uint32_t command_size = command_record_size + command_tail_size;

// TASK_STEPS holds one key, so one string field.
uint32_t step_record_size(uint32_t key_size) {
    return work::task_steps_record_fixed_size + durability::string_field_size(key_size);
}

void write_stamp(uint8_t *at, uint64_t transition_id, uint64_t commit_seq) {
    foundation::store_le64(at, transition_id);
    foundation::store_le64(at + stamp_commit_seq_at, commit_seq);
}

foundation::TransitionStamp read_stamp(const uint8_t *at) {
    return {foundation::load_le64(at), foundation::load_le64(at + stamp_commit_seq_at)};
}

uint32_t maximum(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

template <typename Restore> durability::BatchCheck check_of(Restore value) {
    switch (value) {
    case Restore::restored:
        return durability::BatchCheck::ok;
    case Restore::no_room:
        return durability::BatchCheck::no_room;
    case Restore::corrupt:
        break;
    }
    return durability::BatchCheck::corrupt;
}

// Set, and not after the snapshot's boundary.
bool valid_stamp(foundation::TransitionStamp stamp, const durability::SnapshotMetadata &metadata) {
    return stamp.transition_id != 0U && stamp.transition_id <= metadata.max_transition_id &&
           stamp.commit_seq != 0U && stamp.commit_seq <= metadata.covers_commit_seq;
}

void count(SnapshotLayout &layout, uint32_t body) {
    layout.payload_length += record_prefix + body;
    ++layout.record_count;
}

} // namespace

uint32_t snapshot_record_max(const KernelCapacities &capacities) {
    uint32_t size = maximum(owner_binding_size, maximum(instance_size, task_size));
    size = maximum(size, work::goal_record_fixed_size + 2U * id_max +
                             capacities.goal_parameter_max_bytes + goal_extra);
    size = maximum(size, step_record_size(id_max));
    size = maximum(size, work::task_checkpoint_record_fixed_size + id_max +
                             capacities.task_checkpoint_max_bytes);
    return record_prefix + maximum(size, command_size);
}

// Every table full, each record at its largest.
uint64_t snapshot_payload_max(const KernelCapacities &capacities) {
    uint64_t size = record_prefix + owner_binding_size;
    size += uint64_t{capacities.agent_capacity} * (record_prefix + instance_size);
    size += uint64_t{capacities.goal_capacity} *
            (record_prefix + work::goal_record_fixed_size + 2U * id_max +
             capacities.goal_parameter_max_bytes + goal_extra);
    size += uint64_t{capacities.task_capacity} *
            (record_prefix + task_size + record_prefix + work::task_state_record_size +
             uint64_t{capacities.task_step_capacity} * (record_prefix + step_record_size(id_max)) +
             record_prefix + work::task_checkpoint_record_fixed_size + id_max +
             capacities.task_checkpoint_max_bytes);
    size += uint64_t{authority::PrincipalRegistry::capacity} * capacities.command_capacity *
            (record_prefix + command_size);
    return size;
}

SnapshotLayout measure_snapshot(const WorkModel &model) {
    SnapshotLayout layout{};
    if (model.principals.binding_known()) {
        count(layout, owner_binding_size);
    }
    for (uint32_t index = 0; index < model.agents.size(); ++index) {
        if (model.agents.at(index).instance_id != 0U) {
            count(layout, instance_size);
        }
    }
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        count(layout, work::goal_record_size(model.goals.at(index)) + goal_extra);
    }
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        const work::Task &task = model.tasks.at(index);
        count(layout, task_size);
        for (uint32_t step = 0; step < task.step_count; ++step) {
            count(layout, step_record_size(model.tasks.step(index, step).size()));
        }
        if (task.checkpoint_writer_version != 0U) {
            count(layout, work::task_checkpoint_record_size(task));
        }
        count(layout, work::task_state_record_size);
    }
    for (uint32_t index = 0; index < model.commands.size(); ++index) {
        count(layout, command_size);
    }
    return layout;
}

void WorkSnapshotWriter::reset() {
    section_ = Section::owner_binding;
    index_ = 0;
    depth_ = 0;
    task_part_ = TaskPart::task;
    step_ = 0;
    command_after_seq_ = 0;
    command_index_ = 0;
}

// The type and size of the next record, without moving past it; false once
// every record is written.
bool WorkSnapshotWriter::next_record(uint16_t &type, uint32_t &size) {
    for (;;) {
        switch (section_) {
        case Section::owner_binding:
            if (model_.principals.binding_known()) {
                type = authority::owner_binding_record_type;
                size = owner_binding_size;
                return true;
            }
            section_ = Section::instances;
            break;
        case Section::instances:
            while (index_ < model_.agents.size() && model_.agents.at(index_).instance_id == 0U) {
                ++index_;
            }
            if (index_ < model_.agents.size()) {
                type = work::instance_record_type;
                size = instance_size;
                return true;
            }
            section_ = Section::goals;
            index_ = 0;
            break;
        case Section::goals:
            // Parents precede children even after table slots have been
            // reused. Depth has a fixed, small upper bound.
            while (index_ < model_.goals.size() && model_.goals.at(index_).depth != depth_) {
                ++index_;
            }
            if (index_ < model_.goals.size()) {
                type = work::goal_record_type;
                size = work::goal_record_size(model_.goals.at(index_)) + goal_extra;
                return true;
            }
            if (depth_ < AYNI_GOAL_DEPTH_MAX_MAX) {
                ++depth_;
                index_ = 0;
                break;
            }
            section_ = Section::tasks;
            index_ = 0;
            break;
        case Section::tasks:
            if (index_ >= model_.tasks.size()) {
                section_ = Section::commands;
                break;
            }
            if (task_part_ == TaskPart::task) {
                type = work::task_record_type;
                size = task_size;
                return true;
            }
            if (task_part_ == TaskPart::steps) {
                if (step_ < model_.tasks.at(index_).step_count) {
                    type = work::task_steps_record_type;
                    size = step_record_size(model_.tasks.step(index_, step_).size());
                    return true;
                }
                task_part_ = TaskPart::checkpoint;
            }
            // A task no strategy wrote a checkpoint for has none.
            if (task_part_ == TaskPart::checkpoint) {
                if (model_.tasks.at(index_).checkpoint_writer_version != 0U) {
                    type = work::task_checkpoint_record_type;
                    size = work::task_checkpoint_record_size(model_.tasks.at(index_));
                    return true;
                }
                task_part_ = TaskPart::state;
            }
            type = work::task_state_record_type;
            size = work::task_state_record_size;
            return true;
        // The command with the next commit_seq; the table keeps no order.
        case Section::commands: {
            bool found = false;
            uint64_t first = UINT64_MAX;
            for (uint32_t index = 0; index < model_.commands.size(); ++index) {
                const CommandMapping &mapping = model_.commands.at(index);
                if (mapping.commit_seq > command_after_seq_ && mapping.commit_seq < first) {
                    first = mapping.commit_seq;
                    command_index_ = index;
                    found = true;
                }
            }
            if (found) {
                type = command_record_type;
                size = command_size;
                return true;
            }
            section_ = Section::done;
            break;
        }
        case Section::done:
            return false;
        }
    }
}

void WorkSnapshotWriter::write_record(uint16_t type, uint8_t *body, uint32_t size) {
    switch (type) {
    case authority::owner_binding_record_type: {
        const authority::OwnerBinding &binding = *model_.principals.binding();
        authority::write_owner_binding_record(binding, body);
        write_stamp(body + authority::owner_binding_record_size, binding.stamp.transition_id,
                    binding.stamp.commit_seq);
        break;
    }
    case work::instance_record_type: {
        const work::AgentInstance &instance = model_.agents.at(index_);
        work::write_instance_record(instance, body);
        write_stamp(body + work::instance_record_size, instance.stamp.transition_id,
                    instance.stamp.commit_seq);
        break;
    }
    case work::goal_record_type: {
        const work::Goal &goal = model_.goals.at(index_);
        const uint32_t base_size = work::goal_record_size(goal);
        work::write_goal_record(goal, model_.goals.parameters(index_), body);
        work::write_goal_state_record(goal, body + base_size);
        write_stamp(body + base_size + work::goal_state_record_size, goal.created_transition_id,
                    goal.commit_seq);
        break;
    }
    case work::task_record_type: {
        const work::Task &task = model_.tasks.at(index_);
        work::write_task_record(task, body);
        durability::BodyWriter writer(body + work::task_record_size, task_tail_size);
        writer.u64(task.created_transition_id);
        writer.u64(task.commit_seq);
        writer.u64(task.steps_seq);
        writer.u64(task.checkpoint_seq);
        writer.u32(task.steps_before);
        break;
    }
    case work::task_steps_record_type: {
        const catalog::CatalogId &key = model_.tasks.step(index_, step_);
        durability::BodyWriter writer(body, size);
        writer.u64(model_.tasks.at(index_).task_id);
        writer.u32(step_); // steps_before: the key's position
        writer.u32(1);     // count
        writer.bytes(key.data(), key.size());
        break;
    }
    case work::task_checkpoint_record_type:
        work::write_task_checkpoint_record(model_.tasks.at(index_), model_.tasks.checkpoint(index_),
                                           body);
        break;
    case work::task_state_record_type:
        work::write_task_state_record(model_.tasks.at(index_), body);
        break;
    case command_record_type: {
        const CommandMapping &mapping = model_.commands.at(command_index_);
        write_command_record(mapping, model_.commands.floor(mapping.principal_id), body);
        foundation::store_le64(body + command_record_size, mapping.commit_seq);
        break;
    }
    default:
        break;
    }
}

void WorkSnapshotWriter::advance_record(uint16_t type) {
    switch (type) {
    case authority::owner_binding_record_type:
        section_ = Section::instances;
        break;
    case work::instance_record_type:
    case work::goal_record_type:
        ++index_;
        break;
    case work::task_record_type:
        task_part_ = TaskPart::steps;
        break;
    case work::task_steps_record_type:
        ++step_;
        break;
    case work::task_checkpoint_record_type:
        task_part_ = TaskPart::state;
        break;
    case work::task_state_record_type:
        ++index_;
        task_part_ = TaskPart::task;
        step_ = 0;
        break;
    case command_record_type:
        command_after_seq_ = model_.commands.at(command_index_).commit_seq;
        break;
    default:
        break;
    }
}

WorkSnapshotWriter::Page WorkSnapshotWriter::next(uint8_t *buffer, uint32_t capacity,
                                                  uint32_t &out_size) {
    out_size = 0;
    uint16_t type = 0;
    uint32_t size = 0;
    while (next_record(type, size)) {
        if (capacity - out_size < record_prefix || capacity - out_size - record_prefix < size) {
            return out_size == 0U ? Page::no_room : Page::page;
        }
        uint8_t *at = buffer + out_size;
        durability::write_record_header(
            {type, snapshot_record_version, durability::record_required, size}, at);
        write_record(type, at + record_prefix, size);
        out_size += record_prefix + size;
        advance_record(type);
    }
    return out_size == 0U ? Page::done : Page::page;
}

void WorkSnapshotReader::reset(const durability::SnapshotMetadata &metadata) {
    metadata_ = metadata;
    records_ = 0;
    section_ = Section::owner_binding;
    task_index_ = work::TaskTable::no_index;
    checkpoint_seen_ = false;
    last_command_seq_ = 0;
}

durability::BatchCheck WorkSnapshotReader::apply(const durability::RecordHeader &header,
                                                 const uint8_t *body) {
    using durability::BatchCheck;
    if (records_ == metadata_.record_count) {
        return BatchCheck::corrupt;
    }
    ++records_;
    if ((header.flags & ~durability::record_required) != 0U) {
        return BatchCheck::unsupported;
    }
    if (WorkReplay::classify_header(header) == durability::RecordUse::corrupt) {
        return BatchCheck::corrupt;
    }
    // A record this reader does not know is skipped unless REQUIRED.
    // GOAL_STATE travels inside GOAL, so a snapshot has none of its own.
    const BatchCheck unknown = (header.flags & durability::record_required) == 0U
                                   ? BatchCheck::ok
                                   : BatchCheck::unsupported;
    if (header.version != snapshot_record_version) {
        return unknown;
    }
    Section section = Section::instances;
    switch (header.type) {
    case authority::owner_binding_record_type:
        section = Section::owner_binding;
        break;
    case work::instance_record_type:
        section = Section::instances;
        break;
    case work::goal_record_type:
        section = Section::goals;
        break;
    case work::task_record_type:
    case work::task_state_record_type:
    case work::task_steps_record_type:
    case work::task_checkpoint_record_type:
        section = Section::tasks;
        break;
    case command_record_type:
        section = Section::commands;
        break;
    default:
        return unknown;
    }
    // Sections come in order, and a task's records together.
    if (section < section_ ||
        (task_index_ != work::TaskTable::no_index && section != Section::tasks)) {
        return BatchCheck::corrupt;
    }
    section_ = section;
    switch (header.type) {
    case authority::owner_binding_record_type:
        return owner_binding(body, header.length);
    case work::instance_record_type:
        return instance(body, header.length);
    case work::goal_record_type:
        return goal(body, header.length);
    case work::task_record_type:
        return task(body, header.length);
    case work::task_steps_record_type:
        return task_steps(body, header.length);
    case work::task_checkpoint_record_type:
        return task_checkpoint(body, header.length);
    case work::task_state_record_type:
        return task_state(body, header.length);
    case command_record_type:
        return command(body, header.length);
    default:
        return BatchCheck::unsupported;
    }
}

durability::BatchCheck WorkSnapshotReader::owner_binding(const uint8_t *body, uint32_t size) {
    authority::OwnerBindingRecord record{};
    if (size != owner_binding_size ||
        !authority::read_owner_binding_record(body, authority::owner_binding_record_size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_stamp(body + authority::owner_binding_record_size);
    return valid_stamp(stamp, metadata_) && model_.principals.restore_binding(record, stamp)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}

durability::BatchCheck WorkSnapshotReader::instance(const uint8_t *body, uint32_t size) {
    work::InstanceRecord record{};
    if (size != instance_size ||
        !work::read_instance_record(body, work::instance_record_size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp = read_stamp(body + work::instance_record_size);
    if (!valid_stamp(stamp, metadata_) ||
        model_.agents.find_instance(record.instance_id) != nullptr) {
        return durability::BatchCheck::corrupt;
    }
    return check_of(model_.agents.restore(record.agent_id, record.instance_id, record.revision,
                                          record.lifecycle, stamp));
}

durability::BatchCheck WorkSnapshotReader::goal(const uint8_t *body, uint32_t size) {
    if (size < work::goal_record_fixed_size + goal_extra) {
        return durability::BatchCheck::corrupt;
    }
    const uint32_t base_size = size - goal_extra;
    work::GoalRecord record{};
    work::GoalStateRecord state{};
    if (!work::read_goal_record(body, base_size, record) ||
        !work::read_goal_state_record(body + base_size, work::goal_state_record_size, state) ||
        record.goal_id != state.goal_id ||
        model_.agents.find_instance(record.goal.owner_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_stamp(body + base_size + work::goal_state_record_size);
    if (!valid_stamp(stamp, metadata_)) {
        return durability::BatchCheck::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    return check_of(model_.goals.restore_snapshot(record.goal, record.goal_id,
                                                  record.replaced_goal_id, record.depth,
                                                  state.state, stamp, index));
}

durability::BatchCheck WorkSnapshotReader::task(const uint8_t *body, uint32_t size) {
    work::TaskRecord record{};
    if (task_index_ != work::TaskTable::no_index || size != task_size ||
        !work::read_task_record(body, work::task_record_size, record)) {
        return durability::BatchCheck::corrupt;
    }
    uint32_t goal_index = 0;
    if (!model_.goals.find(record.task.goal_id, goal_index) ||
        model_.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return durability::BatchCheck::corrupt;
    }
    durability::BodyReader reader(body + work::task_record_size, task_tail_size);
    const foundation::TransitionStamp stamp{reader.u64(), reader.u64()};
    const uint64_t steps_seq = reader.u64();
    const uint64_t checkpoint_seq = reader.u64();
    const uint32_t steps_before = reader.u32();
    if (!valid_stamp(stamp, metadata_) || !reader.ok()) {
        return durability::BatchCheck::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore =
        model_.tasks.restore_snapshot(record.task, record.task_id, record.replaced_task_id, stamp,
                                      steps_seq, checkpoint_seq, steps_before);
    if (restore == work::TaskTable::Restore::restored) {
        static_cast<void>(model_.tasks.find(record.task_id, task_index_));
        checkpoint_seen_ = false;
    }
    return check_of(restore);
}

durability::BatchCheck WorkSnapshotReader::task_steps(const uint8_t *body, uint32_t size) {
    if (task_index_ == work::TaskTable::no_index || checkpoint_seen_) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model_.tasks.at(task_index_);
    if (task.steps_seq == 0U) {
        return durability::BatchCheck::corrupt;
    }
    uint64_t task_id = 0;
    work::TaskTable::Restore restore = work::TaskTable::Restore::corrupt;
    uint32_t seen = 0;
    const bool read =
        work::read_task_steps_record(body, size, task_id, [&](uint32_t step, ayni_string_v1 key) {
            if (task_id != task.task_id || seen++ != 0U) {
                return false;
            }
            restore = model_.tasks.restore_step(task_index_, step, key, {0, task.steps_seq});
            return restore == work::TaskTable::Restore::restored;
        });
    return read ? durability::BatchCheck::ok
                : check_of(restore == work::TaskTable::Restore::no_room
                               ? restore
                               : work::TaskTable::Restore::corrupt);
}

durability::BatchCheck WorkSnapshotReader::task_checkpoint(const uint8_t *body, uint32_t size) {
    work::TaskCheckpointRecord record{};
    if (task_index_ == work::TaskTable::no_index || checkpoint_seen_ ||
        !work::read_task_checkpoint_record(body, size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model_.tasks.at(task_index_);
    if (record.task_id != task.task_id || task.checkpoint_seq == 0U) {
        return durability::BatchCheck::corrupt;
    }
    const auto restore =
        model_.tasks.restore_checkpoint(task_index_, record.writer_id, record.writer_version,
                                        record.state, {0, task.checkpoint_seq});
    checkpoint_seen_ = restore == work::TaskTable::Restore::restored;
    return check_of(restore);
}

durability::BatchCheck WorkSnapshotReader::task_state(const uint8_t *body, uint32_t size) {
    work::TaskStateRecord record{};
    if (task_index_ == work::TaskTable::no_index ||
        !work::read_task_state_record(body, size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model_.tasks.at(task_index_);
    if (record.task_id != task.task_id || task.steps_before > task.step_count ||
        (task.steps_seq == 0U) != (task.step_count == 0U) ||
        (task.checkpoint_seq != 0U) != checkpoint_seen_) {
        return durability::BatchCheck::corrupt;
    }
    const auto restore = model_.tasks.restore_state(task_index_, record.state,
                                                    {task.created_transition_id, task.commit_seq});
    task_index_ = work::TaskTable::no_index;
    return check_of(restore);
}

durability::BatchCheck WorkSnapshotReader::command(const uint8_t *body, uint32_t size) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    if (size != command_size || !read_command_record(body, command_record_size, mapping, floor)) {
        return durability::BatchCheck::corrupt;
    }
    mapping.commit_seq = foundation::load_le64(body + command_record_size);
    if (mapping.commit_seq <= last_command_seq_ ||
        mapping.commit_seq > metadata_.covers_commit_seq) {
        return durability::BatchCheck::corrupt;
    }
    last_command_seq_ = mapping.commit_seq;
    return check_of(model_.commands.restore_checked(mapping, floor));
}

durability::BatchCheck WorkSnapshotReader::complete() const {
    return records_ == metadata_.record_count && task_index_ == work::TaskTable::no_index &&
                   WorkReplay(model_).consistent()
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}

} // namespace ayni::kernel
