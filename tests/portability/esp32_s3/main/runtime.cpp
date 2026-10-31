// Consumer-owned platform composition. All application scenarios are shared with Native.
#include "suite.h"
#include <ayni/hosts/esp_idf.h>
#include <cstdio>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_spi_flash_counters.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace {
using ayni::runtime::portable::Profile;
constexpr uint32_t owner_stack_bytes = 96 * 1024;
constexpr uint32_t stack_headroom_percent = 25;
constexpr uint32_t percent = 100;
constexpr uint32_t owner_priority = 5;
constexpr uint32_t boot_count = 2;
constexpr uint32_t expected_flash_bytes = 16 * 1024 * 1024;
constexpr uint32_t expected_psram_bytes = 8 * 1024 * 1024;
constexpr int64_t fixture_utc = 1893456000;
// Internal static task memory: the application region alone uses PSRAM.
StaticTask_t owner_tcb;
StackType_t owner_stack[owner_stack_bytes / sizeof(StackType_t)];
void utc(void *, ayni_utc_sample_v1 *out) {
    *out = {sizeof(*out), AYNI_UTC_TRUSTED, fixture_utc};
}
void yield(void *) {
    vTaskDelay(1);
}
void heap(const char *stage) {
    for (const uint32_t caps :
         {MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT}) {
        std::printf("heap %s caps=%u free=%zu largest=%zu min=%zu\n", stage,
                    static_cast<unsigned>(caps), heap_caps_get_free_size(caps),
                    heap_caps_get_largest_free_block(caps), heap_caps_get_minimum_free_size(caps));
    }
}
// Failure parks this owner with every stack/arena/Producer borrow still alive.
// It never frees a live Kernel or retries old physical execution commands.
[[noreturn]] void park(ayni_status_t status) {
    std::printf("portable ESP status=%ld; owner parked\n", static_cast<long>(status));
    while (true) {
        vTaskSuspend(nullptr);
    }
}
void owner(void *) {
    ayni::hosts::esp_idf::log_platform_baseline();
    uint32_t flash = 0;
    if (esp_flash_get_physical_size(nullptr, &flash) != ESP_OK || flash != expected_flash_bytes ||
        !esp_psram_is_initialized() || esp_psram_get_size() != expected_psram_bytes) {
        park(AYNI_NOT_READY);
    }
    std::printf("source=%s ABI=%u.%u\n", AYNI_SOURCE_REVISION, AYNI_ABI_MAJOR, AYNI_ABI_MINOR);
    ayni::runtime::portable::Memory memory;
    auto status = Profile::memory_requirements(memory);
    if (status != AYNI_OK) {
        park(status);
    }
    std::printf("portable caller region=%zu kernel=%zu align=%zu; Host/Provider heaps separate\n",
                memory.bytes, memory.kernel_bytes, memory.alignment);
    heap("before allocation");
    void *arena = heap_caps_aligned_alloc(memory.alignment, memory.bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) {
        park(AYNI_CAPACITY_EXCEEDED);
    }
    ayni::portability::Evidence evidence;
    esp_flash_reset_counters();
    for (uint32_t boot = 0; boot < boot_count; ++boot) {
        ayni::hosts::esp_idf::Host host({nullptr, utc});
        const auto opened = host.open_storage("ayni", AYNI_COMPLETION_SLOT_COUNT_DEFAULT,
#ifdef CONFIG_AYNI_INITIALIZE_BLANK
                                              CONFIG_AYNI_INITIALIZE_BLANK
#else
                                              false
#endif
        );
        if (opened.status != ayni::hosts::esp_idf::StorageStatus::ok) {
            std::printf("storage open status=%u cause=%d\n", static_cast<unsigned>(opened.status),
                        opened.error);
            park(AYNI_STORAGE_FAILED);
        }
        const auto ports = host.table();
        Profile profile;
        status = profile.create_in(arena, memory.bytes, ports, host.value_store());
        if (status != AYNI_OK) {
            host.close_storage();
            static_cast<void>(profile.destroy());
            park(status);
        }
        ayni::portability::Policies policies;
        status = ayni::portability::configure(profile, policies);
        const ayni::portability::Clock clock{ports.ctx, ports.monotonic_now_us, yield};
        heap("configured");
        if (status == AYNI_OK) {
            status = ayni::portability::start(profile, clock);
        }
        if (status == AYNI_OK) {
            status = boot == 0 ? ayni::portability::normal(profile, clock, evidence)
                               : ayni::portability::reopened(profile, clock, evidence);
        }
        const auto stopped = ayni::portability::stop(profile, clock);
        if (stopped != AYNI_OK) {
            park(stopped);
        } // Producers still borrow the caller region.
        host.close_storage();
        const auto destroyed = profile.destroy();
        if (destroyed != AYNI_OK) {
            park(destroyed);
        }
        const auto owner_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        const auto worker_free = host.worker_stack_free_bytes();
        std::printf("stack boot=%u owner_free=%u/%u worker_free=%u/%u\n",
                    static_cast<unsigned>(boot), static_cast<unsigned>(owner_free),
                    static_cast<unsigned>(owner_stack_bytes), static_cast<unsigned>(worker_free),
                    static_cast<unsigned>(host.worker_stack_bytes));
        if (owner_free * percent < owner_stack_bytes * stack_headroom_percent ||
            worker_free * percent < host.worker_stack_bytes * stack_headroom_percent) {
            park(AYNI_BUDGET_EXHAUSTED);
        }
        if (status != AYNI_OK) {
            park(status);
        }
    }
    // No application worker remains. Counts cover whole-chip driver operations
    // for this normal run, including blank initialization when explicitly selected.
    // They are neither per-cell wear measurements nor endurance evidence.
    esp_flash_dump_counters(stdout);
    heap_caps_free(arena);
    heap("after joined close");
    park(AYNI_OK);
}
} // namespace
void run_portable_s3() {
    if (!xTaskCreateStatic(owner, "ayni-owner", owner_stack_bytes, nullptr, owner_priority,
                           owner_stack, &owner_tcb)) {
        park(AYNI_CAPACITY_EXCEEDED);
    }
}
