// The work model as the records of a snapshot payload (contracts/storage/
// records.md, ADR-0027 decision 7). The writer pages the one durable model
// out into the storage buffer, whole records only, and the reader restores
// it record by record as recovery reads the payload back; neither keeps a
// second copy. A snapshot record is the log record of its type with a tail
// that keeps the stamps the log had, so READY order, reclamation order and
// command results come back as they were. What only this boot's
// registration changed, such as a fault or hold for something it lacks, is
// not written.

#ifndef AYNI_CORE_KERNEL_WORK_SNAPSHOT_H
#define AYNI_CORE_KERNEL_WORK_SNAPSHOT_H

#include "durability/snapshot/snapshot_codec.h"
#include "kernel/intent/work_model.h"

#include <cstdint>

namespace ayni::kernel {

struct SnapshotLayout {
    uint64_t payload_length;
    uint32_t record_count;
};

// The payload the writer would write for the model now.
SnapshotLayout measure_snapshot(const WorkModel &model);
// The largest record, with its envelope, under these capacities; the
// storage buffer holds at least one.
uint32_t snapshot_record_max(const KernelCapacities &capacities);
// The largest payload these capacities allow.
uint64_t snapshot_payload_max(const KernelCapacities &capacities);

class WorkSnapshotWriter {
  public:
    enum class Page { page, done, no_room };
    explicit WorkSnapshotWriter(const WorkModel &model) : model_(model) {}
    // Writes the next records into buffer, complete records only, and their
    // size into out_size: done when none is left, no_room when the next one
    // alone does not fit. The model stays quiescent until done; the caller
    // retains a page until its asynchronous write completes.
    Page next(uint8_t *buffer, uint32_t capacity, uint32_t &out_size);
    // Starts again from the first record.
    void reset();

  private:
    // The payload's order (records.md).
    enum class Section { instances, goals, tasks, commands, done };
    // A task's records, in order.
    enum class TaskPart { task, steps, checkpoint, state };
    bool next_record(uint16_t &type, uint32_t &size);
    void write_record(uint16_t type, uint8_t *body, uint32_t size);
    void advance_record(uint16_t type);

    const WorkModel &model_;
    Section section_ = Section::instances;
    uint32_t index_ = 0; // of the instance, goal or task in its table
    uint32_t depth_ = 0; // goals go by depth, so parents come first
    TaskPart task_part_ = TaskPart::task;
    uint32_t step_ = 0; // the task's next step key
    // Commands go by the commit_seq that accepted them: the last one
    // written, and the one the next record is of.
    uint64_t command_after_seq_ = 0;
    uint32_t command_index_ = 0;
};

class WorkSnapshotReader {
  public:
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
    enum class Section { instances, goals, tasks, commands };
    durability::BatchCheck instance(const uint8_t *body, uint32_t size);
    durability::BatchCheck goal(const uint8_t *body, uint32_t size);
    durability::BatchCheck task(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_steps(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_checkpoint(const uint8_t *body, uint32_t size);
    durability::BatchCheck task_state(const uint8_t *body, uint32_t size);
    durability::BatchCheck command(const uint8_t *body, uint32_t size);

    WorkModel &model_;
    durability::SnapshotMetadata metadata_;
    uint32_t records_ = 0;
    Section section_ = Section::instances;
    // The task whose records come, from TASK up to its TASK_STATE, and
    // whether its checkpoint came.
    uint32_t task_index_ = work::TaskTable::no_index;
    bool checkpoint_seen_ = false;
    uint64_t last_command_seq_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_WORK_SNAPSHOT_H
