#include "chat.h"
#include <cstdio>
#include <cstring>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <new>
namespace {
constexpr uint32_t owner_stack_bytes = 96 * 1024, io_stack_bytes = 8192;
StaticTask_t owner_tcb, io_tcb;
StackType_t owner_stack[owner_stack_bytes / sizeof(StackType_t)],
    io_stack[io_stack_bytes / sizeof(StackType_t)];
void io(void *p) {
    static_cast<usbchat::Transport *>(p)->run();
}
void owner(void *) {
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
    int64_t hello_at = 0;
    for (;;) {
        if (!configured && esp_timer_get_time() - hello_at > 1000000) {
            const bool low = usbchat::MotorOutput::pins_disabled();
            const char *hello =
                low ? "AYNI USB CHAT/3; configure required; motor_pins=low"
                    : "AYNI USB CHAT/3; motor pin readback abnormal; cut driver VIN";
            if (!low) {
                usbchat::MotorOutput::coast_pins();
            }
            transport->send(usbchat::Kind::hello, 0, hello, std::strlen(hello));
            hello_at = esp_timer_get_time();
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
            const auto rc = chat->tick();
            // A worker join is asynchronous; BUSY keeps the owner alive for the next tick.
            if (rc != AYNI_OK && rc != AYNI_BUSY) {
                char s[64];
                std::snprintf(s, sizeof(s), "kernel stopped/held=%ld", long(rc));
                transport->send(usbchat::Kind::status, 0, s, std::strlen(s));
                chat->stop_outputs();
                failed = true;
            }
        }
        vTaskDelay(1);
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
