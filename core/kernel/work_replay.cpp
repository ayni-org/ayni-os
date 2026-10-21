#include "kernel/work_replay.h"

#include "kernel/command/command_records.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

using durability::RecordUse;

template <typename Restore> RecordUse use_of(Restore restore) {
    switch (restore) {
    case Restore::restored:
        return RecordUse::applied;
    case Restore::no_room:
        return RecordUse::no_room;
    case Restore::corrupt:
        break;
    }
    return RecordUse::corrupt;
}

} // namespace

RecordUse WorkReplay::on_record(void *context, const durability::BatchIdentity &batch,
                                const durability::RecordHeader &header, const uint8_t *body) {
    return static_cast<WorkReplay *>(context)->apply(batch, header, body);
}

RecordUse WorkReplay::apply(const durability::BatchIdentity &batch,
                            const durability::RecordHeader &header, const uint8_t *body) {
    // A batch that created a goal or a task without its state ended before
    // this record.
    if (batch.commit_seq != batch_seq_) {
        if (!complete()) {
            return RecordUse::corrupt;
        }
        batch_seq_ = batch.commit_seq;
    }
    const foundation::TransitionStamp stamp{batch.transition_id, batch.commit_seq};
    const uint32_t size = header.length;
    if (header.version != 1U) {
        return RecordUse::unknown;
    }
    switch (header.type) {
    case work::instance_record_type:
        return restore_instance(body, size, stamp);
    case work::goal_record_type:
        return restore_goal(body, size, stamp);
    case work::goal_state_record_type:
        return restore_goal_state(body, size, stamp);
    case work::task_record_type:
        return restore_task(body, size, stamp);
    case work::task_state_record_type:
        return restore_task_state(body, size, stamp);
    case work::task_steps_record_type:
        return restore_task_steps(body, size, stamp);
    case work::task_checkpoint_record_type:
        return restore_task_checkpoint(body, size, stamp);
    case command_record_type:
        return restore_command(body, size, batch.commit_seq);
    default:
        return RecordUse::unknown;
    }
}

static_assert(work::instance_record_version == 1U && work::goal_record_version == 1U &&
                  work::goal_state_record_version == 1U && work::task_record_version == 1U &&
                  work::task_state_record_version == 1U && work::task_steps_record_version == 1U &&
                  work::task_checkpoint_record_version == 1U && command_record_version == 1U,
              "apply() knows version 1 of each record");

RecordUse WorkReplay::restore_instance(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp) {
    work::InstanceRecord record{};
    if (!work::read_instance_record(body, size, record)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.agents.restore(record.agent_id, record.instance_id, record.revision,
                                        record.lifecycle, stamp));
}

RecordUse WorkReplay::restore_goal(const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::GoalRecord record{};
    if (pending_goal_id_ != 0U || !work::read_goal_record(body, size, record)) {
        return RecordUse::corrupt;
    }
    record.goal.goal_type_index = work::GoalTable::no_index;
    uint32_t index = 0;
    const work::GoalTable::Restore restore =
        model_.goals.restore(record.goal, record.goal_id, record.replaced_goal_id, stamp, index);
    if (restore != work::GoalTable::Restore::restored) {
        return use_of(restore);
    }
    // The goal that gave way took its tasks, which name the place, with it
    // (work::insert_goal).
    if (record.replaced_goal_id != 0U) {
        model_.tasks.remove_goal(index);
    }
    if (model_.goals.at(index).depth != record.depth) {
        return RecordUse::corrupt;
    }
    pending_goal_id_ = record.goal_id;
    return RecordUse::applied;
}

RecordUse WorkReplay::restore_goal_state(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::GoalStateRecord record{};
    uint32_t index = 0;
    if (!work::read_goal_state_record(body, size, record) ||
        !model_.goals.find(record.goal_id, index)) {
        return RecordUse::corrupt;
    }
    if (record.goal_id == pending_goal_id_) {
        pending_goal_id_ = 0;
    }
    return use_of(model_.goals.restore_state(index, record.state, stamp));
}

RecordUse WorkReplay::restore_task(const uint8_t *body, uint32_t size,
                                   foundation::TransitionStamp stamp) {
    work::TaskRecord record{};
    uint32_t goal_index = 0;
    if (pending_task_id_ != 0U || !work::read_task_record(body, size, record) ||
        !model_.goals.find(record.task.goal_id, goal_index) ||
        model_.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return RecordUse::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore =
        model_.tasks.restore(record.task, record.task_id, record.replaced_task_id, stamp);
    if (restore == work::TaskTable::Restore::restored) {
        pending_task_id_ = record.task_id;
    }
    return use_of(restore);
}

RecordUse WorkReplay::restore_task_state(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskStateRecord record{};
    uint32_t index = 0;
    if (!work::read_task_state_record(body, size, record) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    if (record.task_id == pending_task_id_) {
        pending_task_id_ = 0;
    }
    return use_of(model_.tasks.restore_state(index, record.state, stamp));
}

RecordUse WorkReplay::restore_task_steps(const uint8_t *body, uint32_t size,
                                         foundation::TransitionStamp stamp) {
    work::TaskTable &tasks = model_.tasks;
    uint64_t task_id = 0;
    uint32_t index = work::TaskTable::no_index;
    work::TaskTable::Restore failure = work::TaskTable::Restore::corrupt;
    const bool read =
        work::read_task_steps_record(body, size, task_id, [&](uint32_t step, ayni_string_v1 key) {
            if (index == work::TaskTable::no_index && !tasks.find(task_id, index)) {
                return false;
            }
            const work::TaskTable::Restore restore = tasks.restore_step(index, step, key, stamp);
            if (restore != work::TaskTable::Restore::restored) {
                failure = restore;
                return false;
            }
            return true;
        });
    return read ? RecordUse::applied : use_of(failure);
}

RecordUse WorkReplay::restore_task_checkpoint(const uint8_t *body, uint32_t size,
                                              foundation::TransitionStamp stamp) {
    work::TaskCheckpointRecord record{};
    uint32_t index = 0;
    if (!work::read_task_checkpoint_record(body, size, record) ||
        !model_.tasks.find(record.task_id, index)) {
        return RecordUse::corrupt;
    }
    return use_of(model_.tasks.restore_checkpoint(index, record.writer_id, record.writer_version,
                                                  record.state, stamp));
}

// A principal is registered anew each boot; the mappings of one this boot
// did not register stay in the log, where none of its sessions can reach
// them.
RecordUse WorkReplay::restore_command(const uint8_t *body, uint32_t size, uint64_t commit_seq) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    if (!read_command_record(body, size, mapping, floor)) {
        return RecordUse::corrupt;
    }
    if (model_.principals.find(mapping.principal_id) == nullptr) {
        return RecordUse::applied;
    }
    mapping.commit_seq = commit_seq;
    return model_.commands.restore(mapping, floor) ? RecordUse::applied : RecordUse::corrupt;
}

void WorkReplay::finish(uint64_t now_us) {
    model_.agents.fault_unregistered();
    model_.goals.resolve_types(
        [this](const catalog::CatalogId &id, uint32_t version, uint32_t &out_index) {
            const catalog::GoalTypeRegistry::Lookup type =
                model_.goal_types.find({id.data(), id.size()}, version);
            switch (type.kind) {
            case catalog::GoalTypeRegistry::Lookup::Kind::found:
                out_index = type.index;
                return AYNI_REASON_NONE;
            case catalog::GoalTypeRegistry::Lookup::Kind::unknown_version:
                return AYNI_REASON_GOAL_TYPE_VERSION;
            case catalog::GoalTypeRegistry::Lookup::Kind::unknown_id:
                break;
            }
            return AYNI_REASON_GOAL_TYPE_UNKNOWN;
        });
    hold_tasks();
    model_.goals.reanchor(now_us);
    model_.tasks.reanchor(now_us);
    model_.due_work_changed = true;
}

// The strategy that decides a task is its goal type's, and only the
// strategy that wrote a checkpoint, at that version, can read it
// (C-POLICY-010).
void WorkReplay::hold_tasks() {
    work::TaskTable &tasks = model_.tasks;
    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const work::Task &task = tasks.at(index);
        if (!work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        const work::Goal &goal = model_.goals.at(task.goal_index);
        if (work::GoalTable::is_held(goal)) {
            tasks.hold_restored(index,
                                goal.hold_reason == AYNI_REASON_GOAL_TYPE_UNKNOWN
                                    ? AYNI_NOT_FOUND
                                    : AYNI_UNSUPPORTED_VERSION,
                                goal.hold_reason);
            continue;
        }
        const catalog::GoalType &type = model_.goal_types.at(goal.goal_type_index);
        if (type.strategy_index == catalog::GoalType::no_strategy) {
            // The type no longer has tasks to decide.
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
            continue;
        }
        const catalog::Strategy &strategy = model_.strategies.at(type.strategy_index);
        if (task.checkpoint_writer_version == 0U) {
            continue;
        }
        if (strategy.version != task.checkpoint_writer_version ||
            !strategy.id.equals(
                {task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size()})) {
            tasks.hold_restored(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_STRATEGY_CHANGED);
        } else if (task.checkpoint_size > strategy.max_checkpoint_bytes) {
            tasks.hold_restored(index, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT);
        }
    }
}

void WorkReplay::discard() {
    model_.agents.drop_restored();
    model_.goals.clear();
    model_.tasks.clear();
    model_.commands.clear();
    batch_seq_ = 0;
    pending_goal_id_ = 0;
    pending_task_id_ = 0;
}

} // namespace ayni::kernel
