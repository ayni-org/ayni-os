#include "kernel/work_journal.h"

#include "catalog/catalog_id.h"
#include "durability/batch/record_body.h"
#include "durability/journal/journal_ring.h"
#include "kernel/command/command_records.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

constexpr uint32_t id_max = catalog::CatalogId::max_size;

// One record with a body of body_size bytes.
constexpr uint32_t record_bytes(uint32_t body_size) {
    return static_cast<uint32_t>(durability::record_header_size) + body_size;
}

durability::RecordHeader header(uint16_t type, uint16_t version, uint32_t length) {
    return {type, version, durability::record_required, length};
}

class Encoder {
  public:
    Encoder(const WorkModel &model, foundation::TransitionStamp stamp,
            durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer) {}

    // Records in replay order: instances, then the goal created, before the
    // states of the goals, then the task created, the steps and checkpoint
    // of the task decided, before the states of the tasks, then the command.
    bool encode() { return instances() && goals() && tasks() && command(); }

  private:
    uint8_t *body(uint16_t type, uint16_t version, uint32_t length) {
        uint8_t *at = writer_.add_record_body(header(type, version, length));
        ok_ = ok_ && at != nullptr;
        return at;
    }

    bool instances() {
        for (uint32_t index = 0; index < model_.agents.size(); ++index) {
            const work::AgentInstance &instance = model_.agents.at(index);
            if (instance.stamp.commit_seq != stamp_.commit_seq) {
                continue;
            }
            uint8_t *at = body(work::instance_record_type, work::instance_record_version,
                               work::instance_record_size);
            if (at != nullptr) {
                work::write_instance_record(instance, at);
            }
        }
        return ok_;
    }

    bool goals() {
        for (uint32_t index = 0; index < model_.goals.size(); ++index) {
            const work::Goal &goal = model_.goals.at(index);
            if (goal.commit_seq != stamp_.commit_seq ||
                goal.created_transition_id != stamp_.transition_id) {
                continue;
            }
            uint8_t *at = body(work::goal_record_type, work::goal_record_version,
                               work::goal_record_size(goal));
            if (at != nullptr) {
                work::write_goal_record(goal, model_.goals.parameters(index), at);
            }
        }
        for (uint32_t index = 0; index < model_.goals.size(); ++index) {
            const work::Goal &goal = model_.goals.at(index);
            if (goal.commit_seq != stamp_.commit_seq) {
                continue;
            }
            uint8_t *at = body(work::goal_state_record_type, work::goal_state_record_version,
                               work::goal_state_record_size);
            if (at != nullptr) {
                work::write_goal_state_record(goal, at);
            }
        }
        return ok_;
    }

    bool tasks() {
        const work::TaskTable &tasks = model_.tasks;
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.commit_seq != stamp_.commit_seq ||
                task.created_transition_id != stamp_.transition_id) {
                continue;
            }
            uint8_t *at =
                body(work::task_record_type, work::task_record_version, work::task_record_size);
            if (at != nullptr) {
                work::write_task_record(task, at);
            }
        }
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.steps_seq == stamp_.commit_seq) {
                uint8_t *at = body(work::task_steps_record_type, work::task_steps_record_version,
                                   work::task_steps_record_size(tasks, index));
                if (at != nullptr) {
                    work::write_task_steps_record(tasks, index, at);
                }
            }
            if (task.checkpoint_seq == stamp_.commit_seq) {
                uint8_t *at =
                    body(work::task_checkpoint_record_type, work::task_checkpoint_record_version,
                         work::task_checkpoint_record_size(task));
                if (at != nullptr) {
                    work::write_task_checkpoint_record(task, tasks.checkpoint(index), at);
                }
            }
        }
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.commit_seq != stamp_.commit_seq) {
                continue;
            }
            uint8_t *at = body(work::task_state_record_type, work::task_state_record_version,
                               work::task_state_record_size);
            if (at != nullptr) {
                work::write_task_state_record(task, at);
            }
        }
        return ok_;
    }

    bool command() {
        const CommandMapping *mapping = model_.commands.mapped_in(stamp_.commit_seq);
        if (mapping == nullptr) {
            return ok_;
        }
        uint8_t *at = body(command_record_type, command_record_version, command_record_size);
        if (at != nullptr) {
            write_command_record(*mapping, model_.commands.floor(mapping->principal_id), at);
        }
        return ok_;
    }

    const WorkModel &model_;
    foundation::TransitionStamp stamp_;
    durability::BatchWriter &writer_;
    bool ok_ = true;
};

} // namespace

uint32_t transition_batch_max(const KernelCapacities &capacities) {
    const uint32_t steps = capacities.task_step_capacity < capacities.policy_intent_capacity
                               ? capacities.task_step_capacity
                               : capacities.policy_intent_capacity;
    uint32_t size =
        static_cast<uint32_t>(durability::batch_header_size + durability::commit_marker_size);
    size += capacities.agent_capacity * record_bytes(work::instance_record_size);
    size += record_bytes(work::goal_record_fixed_size + 2U * id_max +
                         capacities.goal_parameter_max_bytes);
    size += capacities.goal_capacity * record_bytes(work::goal_state_record_size);
    size += record_bytes(work::task_record_size);
    size += record_bytes(work::task_steps_record_fixed_size +
                         steps * durability::string_field_size(id_max));
    size += record_bytes(work::task_checkpoint_record_fixed_size + id_max +
                         capacities.task_checkpoint_max_bytes);
    size += capacities.task_capacity * record_bytes(work::task_state_record_size);
    size += record_bytes(command_record_size);
    return size;
}

uint32_t storage_buffer_min(const KernelCapacities &capacities) {
    return transition_batch_max(capacities) + durability::JournalRing::entry_overhead;
}

std::size_t encode_transition(const WorkModel &model, foundation::TransitionStamp stamp,
                              durability::BatchWriter &writer) {
    Encoder encoder(model, stamp, writer);
    return encoder.encode() ? writer.finish() : 0U;
}

} // namespace ayni::kernel
