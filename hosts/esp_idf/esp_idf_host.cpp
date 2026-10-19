#include <ayni/hosts/esp_idf.h>

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

namespace ayni::hosts::esp_idf {
namespace {

constexpr const char *tag = "ayni.host";

uint64_t monotonic_now_us(void * /*ctx*/) {
    // esp_timer_get_time() is int64_t microseconds since boot and never negative.
    return static_cast<uint64_t>(esp_timer_get_time());
}

const char *chip_model_name(esp_chip_model_t model) {
    switch (model) {
    case CHIP_ESP32S3:
        return "ESP32-S3";
    default:
        return "unexpected (this firmware targets ESP32-S3)";
    }
}

void log_chip() {
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    ESP_LOGI(tag, "chip %s rev v%u.%u, %u core(s), features 0x%08" PRIx32,
             chip_model_name(chip.model), static_cast<unsigned>(chip.revision / 100U),
             static_cast<unsigned>(chip.revision % 100U), static_cast<unsigned>(chip.cores),
             chip.features);
    ESP_LOGI(tag, "ESP-IDF %s, reset reason %d", esp_get_idf_version(),
             static_cast<int>(esp_reset_reason()));
}

void log_flash_and_psram() {
    uint32_t flash_bytes = 0;
    const esp_err_t flash_status = esp_flash_get_physical_size(nullptr, &flash_bytes);
    if (flash_status == ESP_OK) {
        ESP_LOGI(tag, "flash detected %" PRIu32 " bytes", flash_bytes);
    } else {
        ESP_LOGE(tag, "flash size query failed: %s", esp_err_to_name(flash_status));
    }
    if (esp_psram_is_initialized()) {
        ESP_LOGI(tag, "PSRAM initialised %u bytes", static_cast<unsigned>(esp_psram_get_size()));
    } else {
        ESP_LOGW(tag, "PSRAM not initialised; check the module marking and sdkconfig");
    }
}

void log_heap(const char *name, uint32_t caps) {
    ESP_LOGI(tag, "heap %-8s total %u free %u min_free %u largest %u", name,
             static_cast<unsigned>(heap_caps_get_total_size(caps)),
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
}

} // namespace

ayni_host_v1 host_table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = nullptr;
    host.monotonic_now_us = &monotonic_now_us;
    return host;
}

void log_platform_baseline() {
    log_chip();
    log_flash_and_psram();
    log_heap("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap("dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    log_heap("psram", MALLOC_CAP_SPIRAM);
    ESP_LOGI(tag, "task stack high-water mark %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

} // namespace ayni::hosts::esp_idf
