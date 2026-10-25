#include "kernel/work_snapshot.h"

#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "durability/batch/record_body.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"
#include "kernel/work_replay.h"
#include "work/agent/agent_records.h"
#include "work/authority/authority_cleanup_records.h"
#include "work/call/call_records.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

// Schema 2 Goals retain full creation stamps in v4 snapshots. Grants use
// their context-specific versions; exact authority commands retain v5 input
// and results (contracts/storage/records.md).
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
// GOAL v4: GOAL_STATE, immutable creation stamp, then the current sequence.
// Keeping both proves later authorization bindings require a durable handoff.
constexpr uint32_t goal_current_seq_at = work::goal_state_record_size + stamp_size;
constexpr uint32_t goal_extra = goal_current_seq_at + sizeof(uint64_t);
// COMMAND_MAPPING: the commit_seq of the batch that accepted it.
constexpr uint32_t command_tail_size = 8;
uint32_t command_size(const CommandMapping &mapping) {
    return command_record_size_for(mapping) + command_tail_size;
}
uint32_t command_max(const KernelCapacities &capacities) {
    return command_record_max(exact_command_max_bytes(capacities.authority_scope_max_bytes,
                                                      capacities.goal_parameter_max_bytes,
                                                      capacities.goal_requirement_capacity)) +
           command_tail_size;
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

bool matches_retained_revocation(const CommandMapping &mapping, const authority::Grant &grant) {
    if (mapping.effect == AYNI_INTENT_EFFECT_TARGET_GONE ||
        grant.created_stamp.commit_seq >= mapping.commit_seq ||
        grant.stamp.commit_seq > mapping.commit_seq || grant.revision != mapping.object.revision) {
        return false;
    }
    if (mapping.effect == AYNI_INTENT_EFFECT_CHANGED) {
        // Cleanup changes its own stamp only. A retained explicit revoke
        // result therefore still identifies this target's terminal fact.
        return grant.lifecycle == AYNI_GRANT_REVOKED &&
               authority::is_revocation_root(grant.grant_id, grant.revoked_by_root_id) &&
               grant.stamp.commit_seq == mapping.commit_seq;
    }
    return mapping.effect == AYNI_INTENT_EFFECT_NO_CHANGE &&
           grant.stamp.commit_seq < mapping.commit_seq && authority::terminal(grant);
}

// C-STORE-015/017: parents precede children even when a terminal slot was
// reused. Both retained tables expose immutable depth; the writer owns the
// bounded scan cursor and never changes either table during a snapshot.
template <typename Table>
bool next_at_depth(const Table &table, uint32_t max_depth, uint32_t &depth, uint32_t &index) {
    for (;;) {
        while (index < table.size() && table.at(index).depth != depth) {
            ++index;
        }
        if (index < table.size()) {
            return true;
        }
        if (depth == max_depth) {
            return false;
        }
        ++depth;
        index = 0;
    }
}

void count(SnapshotLayout &layout, uint32_t body) {
    layout.payload_length += record_prefix + body;
    ++layout.record_count;
}

} // namespace

uint32_t snapshot_record_max(const KernelCapacities &capacities) {
    uint32_t size =
        maximum(owner_binding_size, maximum(instance_size, work::task_snapshot_record_size));
    size = maximum(size, work::goal_record_fixed_size + 2U * id_max +
                             capacities.goal_parameter_max_bytes + goal_extra);
    size = maximum(size, work::goal_authority_record_max(capacities.goal_requirement_capacity,
                                                         capacities.authority_scope_max_bytes));
    size = maximum(size, work::task_authority_record_max(capacities.goal_requirement_capacity));
    size = maximum(size, work::authority_cleanup_record_size);
    size = maximum(size, work::task_steps_record_max(1));
    size = maximum(size, work::task_checkpoint_record_max(capacities.task_checkpoint_max_bytes));
    size =
        maximum(size, authority::grant_snapshot_record_max(capacities.authority_scope_max_bytes));
    if (capacities.call_capacity != 0U) {
        size =
            maximum(size, work::call_record_max(
                              {capacities.call_capacity, capacities.authority_scope_max_bytes,
                               capacities.call_input_max_bytes, capacities.call_result_max_bytes}));
    }
    return record_prefix + maximum(size, command_max(capacities));
}

// Every table full, each record at its largest.
uint64_t snapshot_payload_max(const KernelCapacities &capacities) {
    uint64_t size = record_prefix + owner_binding_size;
    size += record_prefix + work::authority_cleanup_record_size;
    size += uint64_t{capacities.agent_capacity} * (record_prefix + instance_size);
    size += uint64_t{capacities.grant_capacity} *
            (record_prefix +
             authority::grant_snapshot_record_max(capacities.authority_scope_max_bytes));
    size += uint64_t{capacities.goal_capacity} *
            (record_prefix + work::goal_record_fixed_size + 2U * id_max +
             capacities.goal_parameter_max_bytes + goal_extra);
    size += uint64_t{capacities.goal_capacity} *
            (record_prefix + work::goal_authority_record_max(capacities.goal_requirement_capacity,
                                                             capacities.authority_scope_max_bytes));
    size +=
        uint64_t{capacities.task_capacity} *
        (record_prefix + work::task_snapshot_record_size + record_prefix +
         work::task_state_call_record_size +
         uint64_t{capacities.task_step_capacity} *
             (record_prefix + work::task_steps_record_max(1)) +
         record_prefix + work::task_checkpoint_record_max(capacities.task_checkpoint_max_bytes));
    size += uint64_t{capacities.task_capacity} *
            (record_prefix + work::task_authority_record_max(capacities.goal_requirement_capacity));
    size +=
        uint64_t{capacities.call_capacity} *
        (record_prefix + work::call_record_max(
                             {capacities.call_capacity, capacities.authority_scope_max_bytes,
                              capacities.call_input_max_bytes, capacities.call_result_max_bytes}));
    size += uint64_t{authority::PrincipalRegistry::capacity} * capacities.command_capacity *
            (record_prefix + command_max(capacities));
    return size;
}

SnapshotLayout measure_snapshot(const WorkModel &model) {
    SnapshotLayout layout{};
    count(layout, work::authority_cleanup_record_size);
    if (model.principals.binding_known()) {
        count(layout, owner_binding_size);
    }
    for (uint32_t index = 0; index < model.agents.size(); ++index) {
        if (model.agents.at(index).instance_id != 0U) {
            count(layout, instance_size);
        }
    }
    for (uint32_t index = 0; index < model.grants.size(); ++index) {
        count(layout, authority::grant_snapshot_record_size(
                          model.grants.at(index),
                          authority::grant_snapshot_record_version_for(model.grants.at(index))));
    }
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        count(layout, work::goal_record_size(model.goals.at(index)) + goal_extra);
        if (model.goal_authority.contains(index, model.goals.at(index).goal_id)) {
            count(layout, work::goal_authority_record_size(model.goal_authority.at(index)));
        }
    }
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        const work::Task &task = model.tasks.at(index);
        count(layout, work::task_snapshot_record_size);
        work::TaskAuthorityRecord authority{};
        if (model.task_authority.read(task.slot, task.task_id, authority)) {
            count(layout, work::task_authority_record_max(authority.count));
        }
        for (uint32_t step = 0; step < task.step_count; ++step) {
            count(layout, work::task_steps_record_size(model.tasks, index, {step, 1}));
        }
        if (task.checkpoint_writer_version != 0U) {
            count(layout, work::task_checkpoint_record_size(task));
        }
        count(layout,
              work::task_state_size_for(work::task_state_version_for(model.tasks.at(index))));
    }
    for (uint32_t index = 0; index < model.calls.capacity(); ++index) {
        if (model.calls.occupied(index)) {
            count(layout, work::call_record_size(model.calls.record(index)));
        }
    }
    for (uint32_t index = 0; index < model.commands.size(); ++index) {
        count(layout, command_size(model.commands.at(index)));
    }
    return layout;
}

void WorkSnapshotWriter::reset() {
    section_ = Section::owner_binding;
    index_ = 0;
    depth_ = 0;
    task_part_ = TaskPart::task;
    goal_part_ = GoalPart::goal;
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
            section_ = Section::grants;
            index_ = 0;
            depth_ = 0;
            break;
        case Section::grants:
            if (next_at_depth(model_.grants, AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX, depth_, index_)) {
                type = authority::grant_record_type;
                size = authority::grant_snapshot_record_size(
                    model_.grants.at(index_),
                    authority::grant_snapshot_record_version_for(model_.grants.at(index_)));
                return true;
            }
            section_ = Section::goals;
            index_ = 0;
            depth_ = 0;
            break;
        case Section::goals:
            if (next_goal_record(type, size)) {
                return true;
            }
            section_ = Section::tasks;
            index_ = 0;
            break;
        case Section::tasks:
            if (next_task_record(type, size)) {
                return true;
            }
            section_ = Section::calls;
            index_ = 0;
            break;
        case Section::calls:
            while (index_ < model_.calls.capacity() && !model_.calls.occupied(index_)) {
                ++index_;
            }
            if (index_ < model_.calls.capacity()) {
                type = work::call_record_type;
                size = work::call_record_size(model_.calls.record(index_));
                return true;
            }
            section_ = Section::cleanup;
            break;
        case Section::cleanup:
            type = work::authority_cleanup_record_type;
            size = work::authority_cleanup_record_size;
            return true;
        case Section::commands:
            if (next_command_record(type, size)) {
                return true;
            }
            section_ = Section::done;
            break;
        case Section::done:
            return false;
        }
    }
}

// Each metadata part follows a known present slot in the frozen model;
// failed page placement leaves both index and part unchanged (C-STORE-011).
bool WorkSnapshotWriter::next_goal_record(uint16_t &type, uint32_t &size) {
    if (!next_at_depth(model_.goals, AYNI_GOAL_DEPTH_MAX_MAX, depth_, index_)) {
        return false;
    }
    if (goal_part_ == GoalPart::authority) {
        type = work::goal_authority_record_type;
        size = work::goal_authority_record_size(model_.goal_authority.at(index_));
    } else {
        type = work::goal_record_type;
        size = work::goal_record_size(model_.goals.at(index_)) + goal_extra;
    }
    return true;
}

// A Task's checkpoint is optional, but its final state must follow all steps;
// selection does not consume the record until its complete bytes fit the page.
bool WorkSnapshotWriter::next_task_record(uint16_t &type, uint32_t &size) {
    if (index_ >= model_.tasks.size()) {
        return false;
    }
    if (task_part_ == TaskPart::authority) {
        const auto record = model_.task_authority.at(model_.tasks.at(index_).slot);
        type = work::task_authority_record_type;
        size = work::task_authority_record_max(record.count);
        return true;
    }
    if (task_part_ == TaskPart::task) {
        type = work::task_record_type;
        size = work::task_snapshot_record_size;
        return true;
    }
    if (task_part_ == TaskPart::steps) {
        if (step_ < model_.tasks.at(index_).step_count) {
            type = work::task_steps_record_type;
            size = work::task_steps_record_size(model_.tasks, index_, {step_, 1});
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
    size = work::task_state_size_for(work::task_state_version_for(model_.tasks.at(index_)));
    return true;
}

// The table keeps no order; select the next original acceptance sequence so
// replay restores the same retry window and eviction floor (ADR-0027).
bool WorkSnapshotWriter::next_command_record(uint16_t &type, uint32_t &size) {
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
        size = command_size(model_.commands.at(command_index_));
        return true;
    }
    return false;
}

void WorkSnapshotWriter::write_task_authority(uint8_t *body) {
    const auto record = model_.task_authority.at(model_.tasks.at(index_).slot);
    work::write_task_authority_record(record, body);
}

void WorkSnapshotWriter::write_record(uint16_t type, uint8_t *body) {
    switch (type) {
    case work::call_record_type:
        work::write_call_record(model_.calls.record(index_), body);
        break;
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
    case authority::grant_record_type: {
        authority::write_grant_snapshot_record(
            model_.grants.at(index_), model_.grants.source_domain(index_),
            model_.grants.scope(index_), body,
            authority::grant_snapshot_record_version_for(model_.grants.at(index_)));
        break;
    }
    case work::goal_record_type: {
        const work::Goal &goal = model_.goals.at(index_);
        const uint32_t base_size = work::goal_record_size(goal);
        work::write_goal_record(goal, model_.goals.parameters(index_), body);
        work::write_goal_state_record(goal, body + base_size);
        write_stamp(body + base_size + work::goal_state_record_size, goal.created.transition_id,
                    goal.created.commit_seq);
        foundation::store_le64(body + base_size + goal_current_seq_at, goal.commit_seq);
        break;
    }
    case work::goal_authority_record_type:
        work::write_goal_authority_record(model_.goal_authority.at(index_), body);
        break;
    case work::task_record_type:
        work::write_task_snapshot_record(model_.tasks.at(index_), body);
        break;
    case work::task_steps_record_type:
        // A snapshot carries one key at its original position per record.
        work::write_task_steps_record(model_.tasks, index_, {step_, 1}, body);
        break;
    case work::task_checkpoint_record_type:
        work::write_task_checkpoint_record(model_.tasks.at(index_), model_.tasks.checkpoint(index_),
                                           body);
        break;
    case work::task_authority_record_type:
        write_task_authority(body);
        break;
    case work::task_state_record_type:
        work::write_task_state_record(model_.tasks.at(index_), body,
                                      work::task_state_version_for(model_.tasks.at(index_)));
        break;
    case work::authority_cleanup_record_type:
        work::write_authority_cleanup_record(model_.authority_cleanup.record(), body);
        break;
    case command_record_type: {
        const CommandMapping &mapping = model_.commands.at(command_index_);
        const uint32_t base_size = command_record_size_for(mapping);
        static_cast<void>(write_command_record(mapping, model_.commands.floor(mapping.principal_id),
                                               model_.commands.canonical(mapping), body, base_size,
                                               model_.commands.authorization_result(mapping)));
        foundation::store_le64(body + base_size, mapping.commit_seq);
        break;
    }
    default:
        break;
    }
}

void WorkSnapshotWriter::advance_record(uint16_t type) {
    switch (type) {
    case work::call_record_type:
        ++index_;
        break;
    case work::authority_cleanup_record_type:
        section_ = Section::commands;
        break;
    case authority::owner_binding_record_type:
        section_ = Section::instances;
        break;
    case work::instance_record_type:
    case authority::grant_record_type:
        ++index_;
        break;
    case work::goal_record_type:
        if (model_.goal_authority.contains(index_, model_.goals.at(index_).goal_id)) {
            goal_part_ = GoalPart::authority;
        } else {
            ++index_;
        }
        break;
    case work::goal_authority_record_type:
        ++index_;
        goal_part_ = GoalPart::goal;
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
        task_part_ = TaskPart::authority;
        break;
    case work::task_authority_record_type:
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
        const uint16_t version =
            type == command_record_type
                ? command_record_version_for(model_.commands.at(command_index_))
            : type == work::goal_record_type &&
                    model_.goal_authority.contains(index_, model_.goals.at(index_).goal_id)
                ? work::goal_creation_stamp_snapshot_version
            : type == work::goal_authority_record_type ? work::goal_authority_record_version
            : type == authority::grant_record_type
                ? authority::grant_snapshot_record_version_for(model_.grants.at(index_))
            : type == work::task_state_record_type
                ? work::task_state_version_for(model_.tasks.at(index_))
                : snapshot_record_version;
        durability::write_record_header({type, version, durability::record_required, size}, at);
        write_record(type, at + record_prefix);
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
    task_stage_ = TaskReadStage::contents;
    last_command_seq_ = 0;
    cleanup_seen_ = false;
    goal_authority_pending_.reset();
}

// Keep record ordering separate from body decoding and restore dispatch.
bool WorkSnapshotReader::record_section(uint16_t type, Section &out) {
    switch (type) {
    case work::call_record_type:
        out = Section::calls;
        break;
    case authority::owner_binding_record_type:
        out = Section::owner_binding;
        break;
    case work::instance_record_type:
        out = Section::instances;
        break;
    case authority::grant_record_type:
        out = Section::grants;
        break;
    case work::goal_record_type:
    case work::goal_authority_record_type:
        out = Section::goals;
        break;
    case work::task_authority_record_type:
    case work::task_record_type:
    case work::task_state_record_type:
    case work::task_steps_record_type:
    case work::task_checkpoint_record_type:
        out = Section::tasks;
        break;
    case command_record_type:
        out = Section::commands;
        break;
    case work::authority_cleanup_record_type:
        out = Section::cleanup;
        break;
    default:
        return false;
    }
    return true;
}

durability::BatchCheck WorkSnapshotReader::apply(const durability::RecordHeader &header,
                                                 const uint8_t *body) {
    using durability::BatchCheck;
    if ((task_stage_ == TaskReadStage::authority &&
         header.type != work::task_authority_record_type) ||
        records_ == metadata_.record_count ||
        (goal_authority_pending_ && header.type != work::goal_authority_record_type)) {
        return BatchCheck::corrupt;
    }
    ++records_;
    if ((header.flags & ~durability::record_required) != 0U) {
        return BatchCheck::unsupported;
    }
    const auto use = WorkReplay::classify_header(header, WorkRecordContext::snapshot);
    if (use == durability::RecordUse::corrupt) {
        return BatchCheck::corrupt;
    }
    // A record this reader does not know is skipped unless REQUIRED.
    // GOAL_STATE travels inside GOAL, so a snapshot has none of its own.
    const BatchCheck unknown = (header.flags & durability::record_required) == 0U
                                   ? BatchCheck::ok
                                   : BatchCheck::unsupported;
    if (use != durability::RecordUse::applied) {
        return unknown;
    }
    Section section = section_;
    if (!record_section(header.type, section)) {
        return unknown;
    }
    // Sections come in order, and a task's records together.
    if (section < section_ ||
        (task_index_ != work::TaskTable::no_index && section != Section::tasks)) {
        return BatchCheck::corrupt;
    }
    section_ = section;
    switch (header.type) {
    case work::call_record_type:
        return call(body, header.length);
    case authority::owner_binding_record_type:
        return owner_binding(body, header.length);
    case work::instance_record_type:
        return instance(body, header.length);
    case authority::grant_record_type:
        return grant(header.version, body, header.length);
    case work::goal_record_type:
        return goal(header.version, body, header.length);
    case work::goal_authority_record_type:
        return goal_authority(body, header.length);
    case work::task_record_type:
        return task(body, header.length);
    case work::task_steps_record_type:
        return task_steps(body, header.length);
    case work::task_checkpoint_record_type:
        return task_checkpoint(body, header.length);
    case work::task_authority_record_type:
        return task_authority(body, header.length);
    case work::task_state_record_type:
        return task_state(body, header.length, header.version);
    case work::authority_cleanup_record_type:
        return authority_cleanup(body, header.length);
    case command_record_type:
        return command(header.version, body, header.length);
    default:
        return BatchCheck::unsupported;
    }
}

// Snapshot rows are independently bounded by the covered durable prefix.
// Cross-row references are proved after every section is restored.
durability::BatchCheck WorkSnapshotReader::call(const uint8_t *body, uint32_t size) {
    work::CallRecord record{};
    uint32_t duplicate = 0;
    if (!work::read_call_record(body, size, record) ||
        record.state.latest.boot_epoch > metadata_.boot_epoch ||
        !valid_stamp(record.state.latest.transition, metadata_) ||
        model_.calls.find(record.identity.call_id, duplicate)) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model_.calls.restore_validated(record);
    return status == AYNI_OK                  ? durability::BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
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

durability::BatchCheck WorkSnapshotReader::grant(uint16_t version, const uint8_t *body,
                                                 uint32_t size) {
    authority::GrantSnapshotRecord record{};
    const auto *binding = model_.principals.binding();
    if (!authority::read_grant_snapshot_record(body, size, record, version) ||
        !valid_stamp(record.created_stamp, metadata_) || !valid_stamp(record.stamp, metadata_) ||
        ((record.cleanup_stamp.transition_id != 0U || record.cleanup_stamp.commit_seq != 0U) &&
         !valid_stamp(record.cleanup_stamp, metadata_)) ||
        binding == nullptr || binding->principal_id != record.create.grant.owner_principal_id ||
        binding->binding_revision != record.create.grant.owner_binding_revision ||
        binding->stamp.commit_seq >= record.created_stamp.commit_seq ||
        binding->stamp.transition_id >= record.created_stamp.transition_id ||
        model_.agents.find_instance(record.create.grant.subject_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    return check_of(model_.grants.restore_snapshot(record));
}

durability::BatchCheck WorkSnapshotReader::goal(uint16_t version, const uint8_t *body,
                                                uint32_t size) {
    if (version != work::goal_creation_stamp_snapshot_version) {
        return durability::BatchCheck::unsupported; // No inferred creation sequence for old
                                                    // snapshots.
    }
    if (size < work::goal_record_fixed_size + goal_extra) {
        return durability::BatchCheck::corrupt;
    }
    const uint32_t base_size = size - goal_extra;
    work::GoalRecord record{};
    work::GoalStateRecord state{};
    if (!work::read_goal_record(body, base_size, record, version) ||
        !work::read_goal_state_record(body + base_size, work::goal_state_record_size, state) ||
        record.goal_id != state.goal_id ||
        model_.agents.find_instance(record.goal.owner_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_stamp(body + base_size + work::goal_state_record_size);
    const auto commit_seq = foundation::load_le64(body + base_size + goal_current_seq_at);
    if (!valid_stamp(stamp, metadata_) || commit_seq < stamp.commit_seq ||
        commit_seq > metadata_.covers_commit_seq) {
        return durability::BatchCheck::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    const auto result = check_of(
        model_.goals.restore_snapshot(record.goal, record.goal_id, record.replaced_goal_id,
                                      record.depth, state.state, stamp, commit_seq, index));
    if (result == durability::BatchCheck::ok && record.authority_required) {
        goal_authority_pending_ = record.goal_id;
    }
    return result;
}

// Snapshot Goal v4 and metadata form one adjacent pair; the metadata keeps its
// own last-change stamp, which can precede the Goal's latest ordinary progress.
durability::BatchCheck WorkSnapshotReader::goal_authority(const uint8_t *body, uint32_t size) {
    work::GoalAuthorityRecord record{};
    uint32_t index = 0;
    if (!goal_authority_pending_ || !work::read_goal_authority_record(body, size, record) ||
        record.goal_id != *goal_authority_pending_ || !model_.goals.find(record.goal_id, index) ||
        !valid_stamp(record.stamp, metadata_) ||
        record.stamp.commit_seq > model_.goals.at(index).commit_seq ||
        record.stamp.transition_id < model_.goals.at(index).created.transition_id) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model_.goal_authority.restore_validated(index, record);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                                : durability::BatchCheck::corrupt;
    }
    goal_authority_pending_.reset();
    return durability::BatchCheck::ok;
}

durability::BatchCheck WorkSnapshotReader::task(const uint8_t *body, uint32_t size) {
    work::TaskSnapshotRecord snapshot{};
    if (task_index_ != work::TaskTable::no_index ||
        !work::read_task_snapshot_record(body, size, snapshot)) {
        return durability::BatchCheck::corrupt;
    }
    auto &record = snapshot.creation;
    uint32_t goal_index = 0;
    if (!model_.goals.find(record.task.goal_id, goal_index) ||
        model_.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return durability::BatchCheck::corrupt;
    }
    if (!valid_stamp(snapshot.stamp, metadata_)) {
        return durability::BatchCheck::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore = model_.tasks.restore_snapshot(
        record.task, record.task_id, record.replaced_task_id, snapshot.stamp, snapshot.steps_seq,
        snapshot.checkpoint_seq, snapshot.steps_before);
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

durability::BatchCheck WorkSnapshotReader::task_state(const uint8_t *body, uint32_t size,
                                                      uint16_t version) {
    work::TaskStateRecord record{};
    if (task_index_ == work::TaskTable::no_index ||
        !work::read_task_state_record(body, size, record, version)) {
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
    task_stage_ = TaskReadStage::authority;
    return check_of(restore);
}

// Live Tasks retain the exact current Goal binding; terminal Tasks can keep
// an earlier proven generation after explicit replacement (ADR-0038).
durability::BatchCheck WorkSnapshotReader::task_authority(const uint8_t *body, uint32_t size) {
    work::TaskAuthorityRecord record{};
    if (task_index_ == work::TaskTable::no_index || task_stage_ != TaskReadStage::authority ||
        !work::read_task_authority_record(body, size, record) ||
        !valid_stamp(record.stamp, metadata_)) {
        return durability::BatchCheck::corrupt;
    }
    const auto &task = model_.tasks.at(task_index_);
    if (record.task_id != task.task_id || record.ceiling.goal_id != task.goal_id ||
        record.stamp.commit_seq > task.commit_seq ||
        record.stamp.transition_id < task.created_transition_id) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model_.task_authority.install_validated(task.slot, record);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                                : durability::BatchCheck::corrupt;
    }
    task_index_ = work::TaskTable::no_index;
    task_stage_ = TaskReadStage::contents;
    return durability::BatchCheck::ok;
}

// Snapshot compaction removes the original state/command adjacency. Recheck
// its durable owner and every fact still provable from a retained Task (ADR-0043).
bool WorkSnapshotReader::matches_task_limit_command(const CommandMapping &mapping,
                                                    ayni_bytes_v1 canonical) const {
    TaskLimitCommand command{};
    const auto *binding = model_.principals.binding();
    if (!read_task_limit_command(canonical, command) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq ||
        !foundation::is_kernel_id(mapping.object.id) ||
        foundation::id_boot_epoch(mapping.object.id) > metadata_.boot_epoch) {
        return false;
    }
    uint32_t index = 0;
    if (!model_.tasks.find(mapping.object.id, index)) {
        // A terminal Task may have given way; its exact retry window still survives.
        return true;
    }
    const auto &task = model_.tasks.at(index);
    if (task.commit_seq < mapping.commit_seq || task.revision < mapping.object.revision ||
        task.model_calls.limit < command.limit) {
        return false;
    }
    return task.commit_seq != mapping.commit_seq ||
           (task.revision == mapping.object.revision && task.model_calls.limit == command.limit &&
            work::TaskTable::is_live(task.lifecycle) && task.model_calls.used < command.limit &&
            !task.model_calls.blocked);
}

// A retained retry result may precede current state; at the same commit it
// must describe exactly that owner's transition, not merely a monotone bound.
bool WorkSnapshotReader::matches_target_command(const CommandMapping &mapping) const {
    const auto *binding = model_.principals.binding();
    if (binding == nullptr || mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq ||
        !foundation::is_kernel_id(mapping.object.id) ||
        foundation::id_boot_epoch(mapping.object.id) > metadata_.boot_epoch) {
        return false;
    }
    uint32_t index = 0;
    if (mapping.kind == AYNI_INTENT_CALL_QUERY_RESULT) {
        if (!model_.calls.find(mapping.object.id, index)) {
            return true; // A resolved call can give way while its command receipt remains.
        }
        const auto &state = model_.calls.state(index);
        if (model_.calls.query_revision(index) < mapping.object.revision ||
            state.latest.transition.commit_seq < mapping.commit_seq ||
            state.created.transition.commit_seq >= mapping.commit_seq) {
            return false;
        }
        return state.latest.transition.commit_seq != mapping.commit_seq ||
               (model_.calls.query_revision(index) == mapping.object.revision && state.query &&
                state.query->phase == work::CallQuery::Phase::requested &&
                state.query->requested.transition.commit_seq == mapping.commit_seq);
    }
    if (!model_.tasks.find(mapping.object.id, index)) {
        // Terminal Tasks may give way while their original command receipts remain.
        return true;
    }
    const auto &task = model_.tasks.at(index);
    if (task.commit_seq < mapping.commit_seq || task.revision < mapping.object.revision) {
        return false;
    }
    // A live selected call was necessarily accepted before the owner could resume its wait.
    if (task.commit_seq != mapping.commit_seq) {
        return true;
    }
    uint32_t call_index = 0;
    return task.revision == mapping.object.revision && work::TaskTable::is_live(task.lifecycle) &&
           task.call.end == work::TaskCall::End::none && task.hold_status == AYNI_OK &&
           task.hold_reason == AYNI_REASON_NONE && model_.calls.find(task.call.id, call_index) &&
           model_.calls.state(call_index).created.transition.commit_seq < mapping.commit_seq;
}

durability::BatchCheck WorkSnapshotReader::command(uint16_t version, const uint8_t *body,
                                                   uint32_t size) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    ayni_bytes_v1 result_bytes{};
    if (size < command_tail_size ||
        !read_command_record(version, body, size - command_tail_size, mapping, floor, canonical,
                             &result_bytes)) {
        return durability::BatchCheck::corrupt;
    }
    mapping.commit_seq = foundation::load_le64(body + size - command_tail_size);
    if (mapping.commit_seq <= last_command_seq_ ||
        mapping.commit_seq > metadata_.covers_commit_seq) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.kind == AYNI_INTENT_GRANT_CREATE || mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        const auto *binding = model_.principals.binding();
        if (binding == nullptr || mapping.principal_id != binding->principal_id ||
            mapping.commit_seq <= binding->stamp.commit_seq ||
            !foundation::is_kernel_id(mapping.object.id) ||
            foundation::id_boot_epoch(mapping.object.id) > metadata_.boot_epoch) {
            return durability::BatchCheck::corrupt;
        }
        uint32_t index = 0;
        if (model_.grants.find(mapping.object.id, index)) {
            const auto &retained = model_.grants.at(index);
            const bool matches =
                mapping.kind == AYNI_INTENT_GRANT_CREATE
                    ? WorkReplay::matches_grant_creation(mapping, canonical, retained,
                                                         model_.grants.source_domain(index),
                                                         model_.grants.scope(index))
                    : matches_retained_revocation(mapping, retained);
            if (!matches) {
                return durability::BatchCheck::corrupt;
            }
        }
    }
    if (is_work_authority_command(mapping.kind) &&
        (foundation::id_boot_epoch(mapping.object.id) > metadata_.boot_epoch ||
         !(mapping.kind == AYNI_INTENT_GOAL_APPROVE
               ? WorkReplay(model_).matches_goal_approval(mapping, canonical, result_bytes,
                                                          metadata_.max_transition_id,
                                                          metadata_.boot_epoch)
               : WorkReplay(model_).matches_goal_reauthorization(mapping, canonical, result_bytes,
                                                                 metadata_.max_transition_id,
                                                                 metadata_.boot_epoch)))) {
        return durability::BatchCheck::corrupt;
    }
    if (mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT &&
        !matches_task_limit_command(mapping, canonical)) {
        return durability::BatchCheck::corrupt;
    }
    if (is_target_command(mapping.kind) && !matches_target_command(mapping)) {
        return durability::BatchCheck::corrupt;
    }
    last_command_seq_ = mapping.commit_seq;
    return check_of(model_.commands.restore_checked(mapping, floor, canonical, result_bytes));
}

durability::BatchCheck WorkSnapshotReader::authority_cleanup(const uint8_t *body, uint32_t size) {
    work::AuthorityCleanupRecord record{};
    if (cleanup_seen_ || !work::read_authority_cleanup_record(body, size, record) ||
        (record.stamp.commit_seq != 0U && !valid_stamp(record.stamp, metadata_)) ||
        foundation::id_boot_epoch(record.after_goal_id) > metadata_.boot_epoch ||
        foundation::id_boot_epoch(record.through_goal_id) > metadata_.boot_epoch ||
        foundation::id_boot_epoch(record.requested_through) > metadata_.boot_epoch) {
        return durability::BatchCheck::corrupt;
    }
    model_.authority_cleanup.restore_validated(record);
    cleanup_seen_ = true;
    return durability::BatchCheck::ok;
}

durability::BatchCheck WorkSnapshotReader::complete() const {
    return records_ == metadata_.record_count && cleanup_seen_ && !goal_authority_pending_ &&
                   task_index_ == work::TaskTable::no_index &&
                   WorkReplay(model_).consistent(metadata_.boot_epoch)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}

} // namespace ayni::kernel
