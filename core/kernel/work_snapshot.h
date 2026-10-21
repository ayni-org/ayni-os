// A snapshot exports the one quiescent, durable work model. It keeps
// original object stamps and excludes this boot's compatibility overlays.

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

SnapshotLayout measure_snapshot(const WorkModel &model);
uint32_t snapshot_record_max(const KernelCapacities &capacities);
uint64_t snapshot_payload_max(const KernelCapacities &capacities);

class WorkSnapshotWriter {
  public:
    enum class Page { page, done, no_room };
    explicit WorkSnapshotWriter(const WorkModel &model) : model_(model) {}
    // Writes complete records only. The model stays quiescent until done;
    // the caller retains a page until its asynchronous write completes.
    Page next(uint8_t *buffer, uint32_t capacity, uint32_t &out_size);
    void reset();

  private:
    enum class Section { instances, goals, tasks, commands, done };
    bool next_record(uint16_t &type, uint32_t &size);
    void write_record(uint16_t type, uint8_t *body, uint32_t size);
    void advance_record(uint16_t type);

    const WorkModel &model_;
    Section section_ = Section::instances;
    uint32_t index_ = 0;
    uint32_t depth_ = 0;
    uint32_t task_part_ = 0;
    uint32_t step_ = 0;
    uint64_t command_after_seq_ = 0;
    uint32_t command_index_ = 0;
};

class WorkSnapshotReader {
  public:
    WorkSnapshotReader(WorkModel &model, const durability::SnapshotMetadata &metadata)
        : model_(model), metadata_(metadata) {}
    void reset(const durability::SnapshotMetadata &metadata);
    // The caller verifies framing and supplies one whole record. Failure
    // leaves recovery unusable: discard it before exposing any state.
    durability::BatchCheck apply(const durability::RecordHeader &header, const uint8_t *body);
    durability::BatchCheck complete() const;

  private:
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
    uint32_t section_ = 0;
    uint32_t task_index_ = work::TaskTable::no_index;
    bool checkpoint_seen_ = false;
    uint64_t last_command_seq_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_WORK_SNAPSHOT_H
