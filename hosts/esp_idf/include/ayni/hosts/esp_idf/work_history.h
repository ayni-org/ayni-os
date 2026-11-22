#ifndef AYNI_HOSTS_ESP_IDF_WORK_HISTORY_H
#define AYNI_HOSTS_ESP_IDF_WORK_HISTORY_H
#include <array>
#include <ayni/hosts/storage/file_value.h>

namespace ayni::hosts::esp_idf {
// One request, recorded before its completion is delivered. The index is the value of
// StorageMetrics::completed before this request. Idle reconciliation has its own counters.
struct StorageWorkSample {
    uint64_t at_us = 0, queue_us = 0, service_us = 0;
    enum class Kind : uint8_t { other, append, read, write, remove } kind = Kind::other;
    storage::FileValueRole role = storage::FileValueRole::other; // Only meaningful for value I/O.
};

// Optional caller-owned memory (the Wi-Fi Chat instance resides in PSRAM). Locking, lifetime
// and reset on open belong to the Host. A missing/overwritten index is detectable by readers.
class StorageWorkHistory {
  public:
    static constexpr uint64_t capacity = 256;
    void reset() { next_ = 0; }
    void record(const StorageWorkSample &sample) {
        samples_[next_ % capacity] = sample;
        ++next_;
    }
    bool read(uint64_t index, StorageWorkSample &sample) const {
        if (index >= next_ || next_ - index > capacity) {
            return false;
        }
        sample = samples_[index % capacity];
        return true;
    }

  private:
    uint64_t next_ = 0;
    std::array<StorageWorkSample, capacity> samples_{};
};
} // namespace ayni::hosts::esp_idf
#endif
