// Test-only partition service: RAM flash with synchronous NOR program/erase rules.
#ifndef AYNI_TEST_ESP_PARTITION_H
#define AYNI_TEST_ESP_PARTITION_H
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_LITTLEFS = 0x83;
struct esp_partition_t {
    uint32_t size;
    bool encrypted;
};
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, std::size_t, void *, std::size_t);
esp_err_t esp_partition_write(const esp_partition_t *, std::size_t, const void *, std::size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *, std::size_t, std::size_t);
#endif
