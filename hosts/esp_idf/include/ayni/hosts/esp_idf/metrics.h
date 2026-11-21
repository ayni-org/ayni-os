// Read-only ESP counters for one open storage instance. No payloads or persistent state.
#ifndef AYNI_HOSTS_ESP_IDF_METRICS_H
#define AYNI_HOSTS_ESP_IDF_METRICS_H
#include <cstdint>
namespace ayni::hosts::esp_idf {
// Worker-owned file API accounting, including seek time; not raw flash device time.
// space_measure_* counts actual lfs_fs_size() traversals, excluding SpaceLedger hits.
struct FileIoMetrics {
    uint64_t read_calls = 0, read_bytes = 0;
    uint64_t read_us = 0, write_us = 0, sync_us = 0;
    uint64_t space_measure_calls = 0, space_measure_us = 0;
};
// Coherent cumulative completed-work snapshot. All durations are wall-clock microseconds,
// including preemption. io includes file read/write/sync; append and value_* are subsets of io.
// idle_space is separate from per-request space. Maxima are lifetime values, not deltas.
struct StorageMetrics {
    uint64_t completed = 0, queue_us = 0, io_us = 0, space_us = 0, post_us = 0;
    uint64_t append_calls = 0, append_us = 0, value_us = 0, scan_frames = 0;
    uint64_t value_read_calls = 0, value_read_us = 0;
    uint64_t value_write_calls = 0, value_write_us = 0;
    uint64_t value_remove_calls = 0, value_remove_us = 0;
    uint64_t idle_space_us = 0, idle_refreshes = 0;
    uint64_t max_queue_us = 0, max_io_us = 0, max_space_us = 0;
    FileIoMetrics files;
};
} // namespace ayni::hosts::esp_idf
#endif
