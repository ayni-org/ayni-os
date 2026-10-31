#pragma once
#include "display.h"
#include "protocol.h"
#include <array>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
namespace usbchat {
class Speaker;
struct Packet {
    Kind kind{};
    uint64_t id = 0;
    uint32_t size = 0;
    std::array<uint8_t, tx_bytes> body{};
};
// Fixed PSRAM lanes; send never waits or allocates. The I/O task alone touches UART/LCD.
class Transport {
  public:
    bool initialize();
    bool send(Kind, uint64_t, const void *, size_t);
    bool receive(Packet &);
    void allow_display(uint64_t call) { display_call_.store(call, std::memory_order_release); }
    void attach_speaker(Speaker *speaker) { speaker_.store(speaker, std::memory_order_release); }
    void run();

  private:
    void consume(uint8_t);
    void dispatch();
    Display display_;
    QueueHandle_t free_ = nullptr, output_ = nullptr, input_ = nullptr;
    std::array<Packet, packet_slots> packets_{};
    std::array<uint8_t, header_bytes> header_{};
    std::array<uint8_t, rx_bytes> body_{};
    size_t header_used_ = 0, body_used_ = 0, body_size_ = 0;
    int64_t last_byte_us_ = 0;
    std::atomic<uint64_t> display_call_{0}; // Only the current authorized document may paint.
    std::atomic<Speaker *> speaker_{
        nullptr}; // Frozen composition, alive through I/O task lifetime.
};
} // namespace usbchat
