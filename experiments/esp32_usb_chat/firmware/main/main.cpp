#include "chat.h"
#include "wake.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <new>
namespace {
constexpr uint32_t owner_stack_bytes = usbchat::owner_stack_capacity, io_stack_bytes = 8192;
// The owner sleeps until a wake or Core's next deadline, never longer than idle_us: device and
// model timeouts the composition checks itself are at most that late. A burst of ready work
// still gives one tick away every burst_us so IDLE and the task watchdog run.
constexpr uint64_t idle_us = 100000, hello_us = 1000000, burst_us = 20000, busy_us = 1000;
StaticTask_t owner_tcb, io_tcb;
StackType_t owner_stack[owner_stack_bytes / sizeof(StackType_t)],
    io_stack[io_stack_bytes / sizeof(StackType_t)];
void io(void *p) {
    static_cast<usbchat::Transport *>(p)->run();
}
void owner(void *) {
    usbchat::owner_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    uint32_t flash = 0;
    if (esp_flash_get_physical_size(nullptr, &flash) != ESP_OK || flash != 16 * 1024 * 1024 ||
        !esp_psram_is_initialized() || esp_psram_get_size() != 8 * 1024 * 1024) {
        std::puts("USB chat memory profile mismatch");
        vTaskSuspend(nullptr);
    }
    auto *region =
        heap_caps_malloc(sizeof(usbchat::Transport), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) {
        std::puts("USB chat transport allocation failed");
        vTaskSuspend(nullptr);
    }
    auto *transport = new (region) usbchat::Transport;
    esp_log_level_set("*", ESP_LOG_NONE); // Serial protocol owns UART after this point.
    if (!transport->initialize() ||
        !xTaskCreateStatic(io, "chat-io", io_stack_bytes, transport, 3, io_stack, &io_tcb)) {
        vTaskSuspend(nullptr);
    }
    // The composition's large protocol lanes use PSRAM; its Host is separately internal.
    auto *chat_region =
        heap_caps_malloc(sizeof(usbchat::Chat), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chat_region) {
        const char s[] = "chat allocation failed";
        transport->send(usbchat::Kind::status, 0, s, sizeof(s) - 1);
        vTaskSuspend(nullptr);
    }
    auto *chat = new (chat_region) usbchat::Chat(*transport);
    auto *packet_region =
        heap_caps_malloc(sizeof(usbchat::Packet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet_region) {
        vTaskSuspend(nullptr);
    }
    auto *packet = new (packet_region) usbchat::Packet;
    bool configured = false, failed = false;
    uint64_t hello_at = 0, burst_at = uint64_t(esp_timer_get_time());
    for (;;) {
        auto now = uint64_t(esp_timer_get_time());
        uint64_t wait_us = idle_us;
        if (!configured) {
            if (!hello_at || now - hello_at >= hello_us) {
                const bool low = usbchat::MotorOutput::pins_disabled();
                const char *hello =
                    low ? "AYNI USB CHAT/4; configure required; motor_pins=low"
                        : "AYNI USB CHAT/4; motor pin readback abnormal; cut driver VIN";
                if (!low) {
                    usbchat::MotorOutput::coast_pins();
                }
                transport->send(usbchat::Kind::hello, 0, hello, std::strlen(hello));
                hello_at = now;
            }
            wait_us = hello_us - (now - hello_at);
        }
        while (transport->receive(*packet)) {
            if (!configured && packet->kind == usbchat::Kind::configure && packet->size == 16) {
                char route[17]{};
                std::memcpy(route, packet->body.data(), 16);
                const auto rc = chat->initialize(route);
                configured = true;
                failed = rc != AYNI_OK;
                if (failed) {
                    char s[64];
                    std::snprintf(s, sizeof(s), "initialize failed=%ld", long(rc));
                    transport->send(usbchat::Kind::status, 0, s, std::strlen(s));
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
                transport->send(usbchat::Kind::status, 0, s, std::strlen(s));
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
                vTaskDelay(1);
                burst_at = uint64_t(esp_timer_get_time());
            }
            continue;
        }
        usbchat::wait_for_wake(wait_us);
        burst_at = uint64_t(esp_timer_get_time());
    }
}
} // namespace
extern "C" void app_main() {
    // Waiting for Mac configuration must already be output-disabled, including failed startup.
    if (usbchat::MotorOutput::coast_pins() != ESP_OK) {
        std::puts("motor boot low-state failed; cut independent driver VIN");
        return;
    }
    xTaskCreateStatic(owner, "chat-owner", owner_stack_bytes, nullptr, 5, owner_stack, &owner_tcb);
}
