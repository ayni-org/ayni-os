#include "chat.h"
#include "motor_output.h"
#include "speaker.h"
#include "wake.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <new>
#include <pthread.h>
namespace {
constexpr uint32_t owner_stack_bytes = wifichat::owner_stack_capacity, io_stack_bytes = 8192;
// The owner sleeps until a wake or Core's next deadline, never longer than idle_us: device and
// model timeouts the composition checks itself are at most that late. A burst of ready work
// still gives one tick away every burst_us so IDLE and the task watchdog run.
constexpr uint64_t idle_us = 100000, hello_us = 1000000, burst_us = 20000, busy_us = 1000;
StaticTask_t io_tcb;
StackType_t io_stack[io_stack_bytes / sizeof(StackType_t)];
void io(void *p) {
    static_cast<wifichat::Transport *>(p)->run();
}
void *owner(void *) {
    wifichat::owner_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    uint32_t flash = 0;
    if (esp_flash_get_physical_size(nullptr, &flash) != ESP_OK || flash != 16 * 1024 * 1024 ||
        !esp_psram_is_initialized() || esp_psram_get_size() != 8 * 1024 * 1024) {
        std::puts("Wi-Fi voice memory profile mismatch");
        vTaskSuspend(nullptr);
    }
    auto *region =
        heap_caps_malloc(sizeof(wifichat::Transport), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) {
        std::puts("Wi-Fi voice transport allocation failed");
        vTaskSuspend(nullptr);
    }
    auto *transport = new (region) wifichat::Transport;
    esp_log_level_set(
        "*", ESP_LOG_NONE); // Only explicit, redacted diagnostics are emitted after this point.
    if (!transport->initialize()) {
        vTaskSuspend(nullptr);
    }
    // The composition's large protocol lanes use PSRAM; its Host is separately internal.
    auto *chat_region =
        heap_caps_malloc(sizeof(wifichat::Chat), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chat_region) {
        std::puts("chat allocation failed");
        vTaskSuspend(nullptr);
    }
    auto *chat = new (chat_region) wifichat::Chat(*transport);
    // Reserve large internal blocks before the UI task starts Wi-Fi/BLE and TLS.
    // This does not arm motion or start its worker. Initialization failures remain explicit.
    const auto reserved = chat->reserve_control_memory();
    if (reserved != AYNI_OK) {
        std::printf("device control memory reservation failed=%ld\n", long(reserved));
    }
    if (!xTaskCreateStatic(io, "chat-io", io_stack_bytes, transport, 3, io_stack, &io_tcb)) {
        vTaskSuspend(nullptr);
    }
    auto *packet_region =
        heap_caps_malloc(sizeof(wifichat::Packet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet_region) {
        vTaskSuspend(nullptr);
    }
    auto *packet = new (packet_region) wifichat::Packet;
    bool configured = false, failed = false;
    uint64_t hello_at = 0, burst_at = uint64_t(esp_timer_get_time());
    for (;;) {
        auto now = uint64_t(esp_timer_get_time());
        uint64_t wait_us = idle_us;
        if (!configured) {
            if (!hello_at || now - hello_at >= hello_us) {
                const bool low = wifichat::MotorOutput::pins_disabled();
                const char *hello = low ? "AYNI WIFI VOICE; configure required; motor_pins=low"
                                        : "AYNI WIFI VOICE; motor pin readback abnormal";
                if (!low) {
                    wifichat::MotorOutput::coast_pins();
                }
                transport->send(wifichat::Kind::hello, 0, hello, std::strlen(hello));
                hello_at = now;
            }
            wait_us = hello_us - (now - hello_at);
        }
        while (transport->receive(*packet)) {
            if (!configured && packet->kind == wifichat::Kind::configure &&
                packet->size == sizeof(wifichat::ModelSelection)) {
                wifichat::ModelSelection selection;
                std::memcpy(&selection, packet->body.data(), sizeof(selection));
                const auto rc = chat->initialize(selection);
                configured = true;
                failed = rc != AYNI_OK;
                if (failed) {
                    char s[64];
                    std::snprintf(s, sizeof(s), "initialize failed=%ld", long(rc));
                    transport->send(wifichat::Kind::status, 0, s, std::strlen(s));
                }
            } else if (configured && !failed) {
                chat->incoming(*packet);
            }
        }
        if (configured && !failed) {
            ayni_step_report_v1 report{};
            bool again = false;
            const auto rc = chat->tick(report, again);
            now = uint64_t(esp_timer_get_time());
            if (rc == AYNI_BUSY) {
                wait_us = busy_us; // A worker join is asynchronous; poll it on the next tick.
            } else if (rc != AYNI_OK) {
                char s[64];
                std::snprintf(s, sizeof(s), "kernel stopped/held=%ld", long(rc));
                transport->send(wifichat::Kind::status, 0, s, std::strlen(s));
                chat->stop_outputs();
                failed = true;
            } else if (report.more_ready || again) {
                wait_us = 0;
            } else if (report.next_deadline_us != UINT64_MAX) {
                wait_us = report.next_deadline_us > now
                              ? std::min(report.next_deadline_us - now, idle_us)
                              : 0;
            }
        }
        if (!wait_us) {
            if (now - burst_at >= burst_us) {
                const auto wait_entered = uint64_t(esp_timer_get_time());
                vTaskDelay(1);
                burst_at = uint64_t(esp_timer_get_time());
                if (configured && !failed) {
                    chat->record_owner_wait(wait_entered, burst_at);
                }
            }
            continue;
        }
        const auto wait_entered = uint64_t(esp_timer_get_time());
        wifichat::wait_for_wake(wait_us);
        burst_at = uint64_t(esp_timer_get_time());
        if (configured && !failed) {
            chat->record_owner_wait(wait_entered, burst_at);
        }
    }
}
} // namespace
extern "C" void app_main() {
    // Waiting for local configuration must already be output-disabled, including failed startup.
    if (wifichat::MotorOutput::coast_pins() != ESP_OK) {
        std::puts("motor boot low-state failed; cut independent driver VIN");
        return;
    }
    if (wifichat::Speaker::park_outputs() != ESP_OK) {
        std::puts("speaker idle pin setup failed");
        return;
    }
    // FileBodies uses std::this_thread::get_id() for its owner contract. IDF
    // pthread_self() is valid only in a pthread, not in an arbitrary FreeRTOS
    // task. Keep the original priority and internal-RAM stack budget.
    auto config = esp_pthread_get_default_config();
    config.stack_size = owner_stack_bytes;
    config.prio = 5;
    config.thread_name = "chat-owner";
    config.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (esp_pthread_set_cfg(&config) != ESP_OK) {
        std::puts("owner thread configuration failed");
        return;
    }
    pthread_t thread{};
    const auto created = pthread_create(&thread, nullptr, owner, nullptr);
    if (created) {
        std::printf("owner thread creation failed=%d\n", created);
        return;
    }
    // The owner has boot lifetime, including after Core STOPPED. It never exits.
    const auto detached = pthread_detach(thread);
    if (detached) {
        std::printf("owner thread detach failed=%d\n", detached);
    }
}
