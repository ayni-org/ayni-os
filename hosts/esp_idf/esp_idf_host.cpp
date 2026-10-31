#include <ayni/hosts/esp_idf.h>

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>

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

Host::Host() : Host(UtcSource{}) {}
Host::Host(UtcSource utc)
    : utc_(utc), ingress_(xSemaphoreCreateMutexStatic(&ingress_storage_)),
      owner_(xTaskGetCurrentTaskHandle()) {}

Host::~Host() {
    close_storage();
    vSemaphoreDelete(ingress_);
}

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &monotonic_now_us;
    host.utc_now = utc_.sample ? &Host::utc_now : nullptr;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    if (storage_) {
        host.storage_append = &Host::storage_append;
        host.storage_read_prefix = &Host::storage_read_prefix;
        host.storage_read_log = &Host::storage_read_log;
        host.storage_fence_writers = &Host::storage_fence_writers;
        host.storage_write_snapshot = &Host::storage_write_snapshot;
        host.storage_read_snapshot = &Host::storage_read_snapshot;
        host.storage_trim_prefix = &Host::storage_trim_prefix;
        host.storage_free_bytes = &Host::storage_free_bytes;
    }
    return host;
}

bool Host::wait_for_wake(uint64_t timeout_us) {
    constexpr uint64_t tick_us = static_cast<uint64_t>(portTICK_PERIOD_MS) * 1000;
    const uint64_t rounded = timeout_us / tick_us + (timeout_us % tick_us != 0);
    const TickType_t ticks =
        rounded >= portMAX_DELAY ? portMAX_DELAY - 1 : static_cast<TickType_t>(rounded);
    return ulTaskNotifyTake(pdTRUE, ticks) != 0;
}

void Host::utc_now(void *context, ayni_utc_sample_v1 *out) {
    const auto &source = static_cast<Host *>(context)->utc_;
    source.sample(source.context, out);
}

void Host::wake(void *ctx) {
    xTaskNotifyGive(static_cast<Host *>(ctx)->owner_);
}

void Host::ingress_lock(void *ctx) {
    // With portMAX_DELAY the take waits until it succeeds.
    static_cast<void>(xSemaphoreTake(static_cast<Host *>(ctx)->ingress_, portMAX_DELAY));
}

void Host::ingress_unlock(void *ctx) {
    static_cast<void>(xSemaphoreGive(static_cast<Host *>(ctx)->ingress_));
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
