#ifndef AYNI_TEST_ESP_LITTLEFS_H
#define AYNI_TEST_ESP_LITTLEFS_H
#include <esp_partition.h>
inline bool esp_littlefs_partition_mounted(const esp_partition_t *) {
    return false;
}
#endif
