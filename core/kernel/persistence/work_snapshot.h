// The work model as the records of a snapshot payload (contracts/storage/
// records.md, ADR-0027 decision 7). The writer pages the one durable model
// out into the storage buffer, whole records only, and the reader restores
// it record by record as recovery reads the payload back; neither keeps a
// second copy. The records keep the log's envelope and types, but their
// bodies have a syntax of their own (records.md): OWNER_BINDING, INSTANCE,
// GOAL, TASK and COMMAND_MAPPING add the stamps or commit_seq the log had,
// so ownership, READY order, reclamation order and command results come back
// as they were. GOAL carries its GOAL_STATE, TASK its steps_before, and
// TASK_STEPS holds one key a record. What only this boot's registration
// changed, such as a fault or hold for something it lacks, is not written.

#ifndef AYNI_CORE_KERNEL_PERSISTENCE_WORK_SNAPSHOT_H
#define AYNI_CORE_KERNEL_PERSISTENCE_WORK_SNAPSHOT_H

#include "durability/snapshot/snapshot_codec.h"
#include "kernel/intent/work_model.h"

#include <cstdint>
#include <optional>

namespace ayni::kernel {

// Exact size of the current retained model, including each record envelope.
struct SnapshotLayout {
    uint64_t payload_length; // Total payload bytes; zero iff there are no records.
    uint32_t record_count;   // Complete records represented by payload_length; zero for empty.
};

// The payload the writer would write for the model now.
SnapshotLayout measure_snapshot(const WorkModel &model);
// The largest record, with its envelope, under these capacities; the
// storage buffer holds at least one.
uint32_t snapshot_record_max(const KernelCapacities &capacities);
// The largest payload these capacities allow.
uint64_t snapshot_payload_max(const KernelCapacities &capacities);

// Owns the cursor that pages one frozen model into complete snapshot records.
class WorkSnapshotWriter {
  public:
    // A nonempty page, end of payload, or a next record larger than the whole buffer.
    enum class Page { page, done, no_room };
    // Borrows the model; the caller keeps it frozen from reset through done.
    explicit WorkSnapshotWriter(const WorkModel &model) : model_(model) {}
    // The payload next() writes for the model now.
    SnapshotLayout measure() const { return measure_snapshot(model_); }
    // Writes the next records into buffer, complete records only, and their
    // size into out_size: done when none is left, no_room when the next one
    // alone does not fit. The model stays quiescent until done; the caller
    // retains a page until its asynchronous write completes.
    Page next(uint8_t *buffer, uint32_t capacity, uint32_t &out_size);
    // Starts again from the first record.
    void reset();

  private:
    // The payload's order (records.md).
    enum class Section {
        owner_binding,
        instances,
        grants,
        goals,
        tasks,
        stores,
        cleanup,
        commands,
        done
    };
    // A task's records, in order.
    enum class TaskPart { task, steps, checkpoint, state, authority };
    // A Goal v2 and its required authority record remain adjacent across pages.
    enum class GoalPart { goal, authority };
    // Locate, but do not consume, the next complete record across all sections.
    bool next_record(uint16_t &type, uint32_t &size);
    // Select the current Task's next step/checkpoint/state; false after the Task table.
    bool next_task_record(uint16_t &type, uint32_t &size);
    // Select the current store's next row, then the next store's; false after the last.
    bool next_store_record(uint16_t &type, uint32_t &size);
    // Select a Goal or its paired metadata without consuming a page cursor.
    bool next_goal_record(uint16_t &type, uint32_t &size);
    // Select the least acceptance sequence after the last written command.
    bool next_command_record(uint16_t &type, uint32_t &size);
    // Encode the selected record into the already sized body buffer.
    void write_record(uint16_t type, uint8_t *body);
    // Selected frozen Task identity plus original update/step/checkpoint stamps.
    // Serialize the already selected frozen Task admission, without another identity check.
    void write_task_authority(uint8_t *body);
    // Consume a record only after its complete bytes were written.
    void advance_record(uint16_t type);

    const WorkModel &model_;
    Section section_ = Section::owner_binding;
    uint32_t index_ = 0; // of the instance, goal, task or store row in its table
    uint32_t depth_ = 0; // goals go by depth, so parents come first
    TaskPart task_part_ = TaskPart::task;
    GoalPart goal_part_ = GoalPart::goal; // Reset when the complete pair is written.
    uint32_t step_ = 0;                   // the task's next step key
    // Commands go by the commit_seq that accepted them: the last one
    // written, and the one the next record is of.
    uint64_t command_after_seq_ = 0;
    uint32_t command_index_ = 0;
    uint32_t store_ = 0; // of persistent_stores, in the stores section
};

// Owns snapshot ordering and pairing checks while restoring into WorkModel.
class WorkSnapshotReader {
  public:
    // Borrows the recovery model and copies the immutable snapshot boundary.
    WorkSnapshotReader(WorkModel &model, const durability::SnapshotMetadata &metadata)
        : model_(model), metadata_(metadata) {}
    // Starts the payload of the slot metadata describes.
    void reset(const durability::SnapshotMetadata &metadata);
    // The caller verifies framing and supplies one whole record. Failure
    // leaves recovery unusable: discard it before exposing any state.
    durability::BatchCheck apply(const durability::RecordHeader &header, const uint8_t *body);
    // After the last record: all the records the header counts came, no task
    // is left without its state, and the tables are consistent.
    durability::BatchCheck complete() const;

  private:
    // The payload's order (records.md).
    enum class Section {
        owner_binding,
        instances,
        grants,
        goals,
        tasks,
        stores,
        cleanup,
        commands
    };
    durability::BatchCheck owner_binding(const uint8_t *body, uint32_t size);
    durability::BatchCheck instance(const uint8_t *body, uint32_t size);
    durability::BatchCheck grant(uint16_t version, const uint8_t *body, uint32_t size);
    durability::BatchCheck goal(uint16_t version, const uint8_t *body, uint32_t size);
    durability::BatchCheck goal_authority(const uint8_t *body, uint32_t size);
    // Maps supported payload record kinds to their monotone section.
    static bool record_section(uint16_t type, Section &out);
    durability::BatchCheck task(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_steps(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_checkpoint(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_authority(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_state(const uint8_t *body, uint32_t size, uint16_t version);
    durability::BatchCheck authority_cleanup(const uint8_t *body, uint32_t size);
    // Snapshot must retain owner/time and monotone Task facts for a historical raise.
    bool matches_task_limit_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) const;
    // Historical query/resume receipts preserve exact same-sequence target outcomes.
    bool matches_target_command(const CommandMapping &mapping) const;
    durability::BatchCheck command(uint16_t version, const uint8_t *body, uint32_t size);

    WorkModel &model_;
    durability::SnapshotMetadata metadata_;
    uint32_t records_ = 0;
    Section section_ = Section::owner_binding;
    uint32_t store_ = 0; // of persistent_stores whose rows come, in order
    // The Task whose records come, from TASK through TASK_AUTHORITY, and
    // whether its checkpoint came.
    uint32_t task_index_ = work::TaskTable::no_index;
    bool checkpoint_seen_ = false;
    // Contents precede state, then exactly one admission closes the current Task.
    enum class TaskReadStage { contents, authority };
    TaskReadStage task_stage_ = TaskReadStage::contents;
    uint64_t last_command_seq_ = 0;
    // Exactly one REQUIRED scan fact, including an explicit initial idle fact.
    bool cleanup_seen_ = false;
    // Goal v2 must be followed immediately by matching metadata, even across pages.
    std::optional<uint64_t> goal_authority_pending_;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_PERSISTENCE_WORK_SNAPSHOT_H
