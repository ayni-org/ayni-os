// Read-only ESP counters for one open storage instance. No payloads or persistent state.
#ifndef AYNI_HOSTS_ESP_IDF_METRICS_H
#define AYNI_HOSTS_ESP_IDF_METRICS_H
#include <array>
#include <ayni/hosts/storage/file_value.h>
#include <cstddef>
#include <cstdint>
namespace ayni::hosts::esp_idf {
// Worker-owned file API accounting, including seek time; not raw flash device time.
// space_measure_* counts actual lfs_fs_size() traversals, excluding SpaceLedger hits.
struct FileIoMetrics {
    uint64_t read_calls = 0, read_bytes = 0;
    uint64_t read_us = 0, write_us = 0, sync_us = 0;
    uint64_t space_measure_calls = 0, space_measure_us = 0;
};
// Logical payload bytes on confirmed read/write, known target bytes on remove (including
// idempotent removal); calls includes all serviced requests, regardless of status.
struct ValueOperationMetrics {
    uint64_t calls = 0, bytes = 0, service_us = 0, completion_us = 0;

    ValueOperationMetrics since(const ValueOperationMetrics &before) const {
        return {calls - before.calls, bytes - before.bytes, service_us - before.service_us,
                completion_us - before.completion_us};
    }
};
struct ValueRoleMetrics {
    ValueOperationMetrics read, write, remove;
    ValueRoleMetrics since(const ValueRoleMetrics &before) const {
        return {read.since(before.read), write.since(before.write), remove.since(before.remove)};
    }
};
using ValueIoMetrics =
    std::array<ValueRoleMetrics, static_cast<size_t>(storage::FileValueRole::count)>;

// Coherent cumulative completed-work snapshot. All durations are wall-clock microseconds,
// including preemption. io includes file read/write/sync; append and value_* are subsets of io.
// Completed service/file counters publish before owner notification so an immediate snapshot on
// completion sees the triggering I/O; post_us is added separately once notification returns.
// idle_space is separate from per-request space. Maxima are lifetime values, not deltas.
struct StorageMetrics {
    uint64_t completed = 0, queue_us = 0, io_us = 0, space_us = 0, post_us = 0;
    uint64_t append_calls = 0, append_us = 0, value_us = 0, scan_frames = 0;
    uint64_t value_read_calls = 0, value_read_us = 0;
    uint64_t value_write_calls = 0, value_write_us = 0;
    uint64_t value_remove_calls = 0, value_remove_us = 0;
    uint64_t idle_space_us = 0, idle_refreshes = 0;
    uint64_t foreground_measures = 0, foreground_measure_us = 0;
    uint64_t idle_measures = 0, idle_measure_us = 0;
    uint64_t max_queue_us = 0, max_io_us = 0, max_space_us = 0;
    FileIoMetrics files;
    ValueIoMetrics values;

    // Same worker boundary for Host totals and caller-specified attribution. Completion time
    // ends at delivery, just before invoking the callback, and includes queue and space refresh.
    void record_value(const storage::FileValueRequest &request,
                      const storage::FileValueResult &result, uint64_t service_us,
                      uint64_t completion_us) {
        using Kind = storage::FileValueRequest::Kind;
        auto index = static_cast<size_t>(request.role);
        if (index >= values.size()) {
            index = static_cast<size_t>(storage::FileValueRole::other);
        }
        auto &role = values[index];
        auto &op = request.kind == Kind::read    ? role.read
                   : request.kind == Kind::write ? role.write
                                                 : role.remove;
        ++op.calls;
        op.service_us += service_us;
        op.completion_us += completion_us;
        if (result.status == AYNI_OK) {
            op.bytes += request.kind == Kind::remove ? request.logical_remove_bytes : result.size;
        }
    }
};
} // namespace ayni::hosts::esp_idf
#endif
