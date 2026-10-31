#include "transport.h"
#include "speaker.h"
#include <algorithm>
#include <cstring>
#include <driver/uart.h>
#include <esp_timer.h>
namespace usbchat {
namespace {
constexpr uart_port_t uart = UART_NUM_0;
constexpr size_t uart_rx_bytes = 16384, io_chunk_bytes = 2048;
constexpr int64_t frame_timeout_us = 10000000;
constexpr uint8_t magic[] = {'A', 'Y', 'N', '1'};
} // namespace
bool Transport::initialize() {
    free_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    output_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    input_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    if (!free_ || !output_ || !input_ || !display_.initialize()) {
        return false;
    }
    for (uint32_t i = 0; i < packet_slots; ++i) {
        xQueueSend(free_, &i, 0);
    }
    uart_config_t config{};
    config.baud_rate = serial_baud;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    return uart_param_config(uart, &config) == ESP_OK &&
           uart_driver_install(uart, uart_rx_bytes, 0, 0, nullptr, 0) == ESP_OK;
}
bool Transport::send(Kind kind, uint64_t id, const void *body, size_t bytes) {
    if (bytes > tx_bytes || (bytes && !body)) {
        return false;
    }
    uint32_t slot = 0;
    if (xQueueReceive(free_, &slot, 0) != pdTRUE) {
        return false;
    }
    auto &packet = packets_[slot];
    packet.kind = kind;
    packet.id = id;
    packet.size = bytes;
    if (bytes) {
        std::memcpy(packet.body.data(), body, bytes);
    }
    if (xQueueSend(output_, &slot, 0) == pdTRUE) {
        return true;
    }
    xQueueSend(free_, &slot, 0);
    return false;
}
bool Transport::receive(Packet &out) {
    uint32_t slot = 0;
    if (xQueueReceive(input_, &slot, 0) != pdTRUE) {
        return false;
    }
    out = packets_[slot];
    xQueueSend(free_, &slot, 0);
    return true;
}
void Transport::dispatch() {
    if (checksum(body_.data(), body_size_) != get32(header_.data() + 20)) {
        return;
    }
    const auto kind = static_cast<Kind>(header_[5]);
    const auto id = get64(header_.data() + 8);
    // Wire directions are separate contracts. In particular an external ACK
    // must never impersonate the locally generated LCD/DMA completion below.
    switch (kind) {
    case Kind::user:
    case Kind::model_text:
    case Kind::model_error:
    case Kind::display:
    case Kind::cancel:
    case Kind::shutdown:
    case Kind::configure:
    case Kind::model_tools:
    case Kind::render_info:
    case Kind::audio_begin:
    case Kind::audio_chunk:
    case Kind::audio_end:
    case Kind::audio_error:
        break;
    default:
        return;
    }
    if (kind == Kind::audio_begin || kind == Kind::audio_chunk || kind == Kind::audio_end ||
        kind == Kind::audio_error) {
        auto *speaker = speaker_.load(std::memory_order_acquire);
        if (speaker) {
            speaker->receive(kind, id, body_.data(), body_size_);
        }
        return;
    }
    if (kind == Kind::display) {
        if (!id || id != display_call_.load(std::memory_order_acquire)) {
            return;
        }
        const uint8_t result = display_.paint(body_.data(), body_size_) ? 1 : 0;
        send(Kind::display_ack, id, &result, sizeof(result));
        uint32_t slot = 0;
        if (xQueueReceive(free_, &slot, pdMS_TO_TICKS(100)) == pdTRUE) {
            auto &packet = packets_[slot];
            packet.kind = Kind::display_ack;
            packet.id = id;
            packet.size = 1;
            packet.body[0] = result;
            if (xQueueSend(input_, &slot, 0) != pdTRUE) {
                xQueueSend(free_, &slot, 0);
            }
        }
        return;
    }
    if (body_size_ > tx_bytes) {
        return;
    }
    // The I/O stack holds no large packet. Queue copies from a reserved outbound lane.
    uint32_t slot = 0;
    if (xQueueReceive(free_, &slot, 0) != pdTRUE) {
        return;
    }
    auto &packet = packets_[slot];
    packet.kind = kind;
    packet.id = id;
    packet.size = body_size_;
    std::memcpy(packet.body.data(), body_.data(), body_size_);
    if (xQueueSend(input_, &slot, 0) != pdTRUE) {
        xQueueSend(free_, &slot, 0);
        const char error[] = "input queue full";
        send(Kind::status, id, error, sizeof(error) - 1);
    }
}
void Transport::consume(uint8_t byte) {
    const auto now = esp_timer_get_time();
    if (last_byte_us_ && now - last_byte_us_ > frame_timeout_us) {
        header_used_ = body_used_ = body_size_ = 0;
    }
    last_byte_us_ = now;
    if (header_used_ < sizeof(magic)) {
        if (byte == magic[header_used_]) {
            header_[header_used_++] = byte;
        } else {
            header_used_ = byte == magic[0] ? 1 : 0;
        }
        return;
    }
    if (header_used_ < header_bytes) {
        header_[header_used_++] = byte;
        if (header_used_ != header_bytes) {
            return;
        }
        body_size_ = get32(header_.data() + 16);
        if (header_[4] != wire_version || header_[6] || header_[7] || body_size_ > rx_bytes) {
            header_used_ = body_used_ = body_size_ = 0;
            return;
        }
        if (body_size_) {
            return;
        }
    } else {
        body_[body_used_++] = byte;
    }
    if (body_used_ == body_size_) {
        dispatch();
        header_used_ = body_used_ = body_size_ = 0;
    }
}
void Transport::run() {
    std::array<uint8_t, io_chunk_bytes> rx{};
    std::array<uint8_t, header_bytes> tx_header{};
    uint32_t slot = 0;
    size_t sent = 0;
    bool transmitting = false;
    for (;;) {
        const int received = uart_read_bytes(uart, rx.data(), rx.size(), 0);
        for (int i = 0; i < received; ++i) {
            consume(rx[i]);
        }
        if (!transmitting && xQueueReceive(output_, &slot, 0) == pdTRUE) {
            auto &p = packets_[slot];
            std::copy(std::begin(magic), std::end(magic), tx_header.begin());
            tx_header[4] = wire_version;
            tx_header[5] = static_cast<uint8_t>(p.kind);
            put64(tx_header.data() + 8, p.id);
            put32(tx_header.data() + 16, p.size);
            put32(tx_header.data() + 20, checksum(p.body.data(), p.size));
            sent = 0;
            transmitting = true;
        }
        if (transmitting) {
            const auto &p = packets_[slot];
            const bool head = sent < header_bytes;
            const auto *data = head ? tx_header.data() + sent : p.body.data() + sent - header_bytes;
            const size_t remaining = head ? header_bytes - sent : p.size + header_bytes - sent;
            if (remaining) {
                const int written =
                    uart_tx_chars(uart, reinterpret_cast<const char *>(data), remaining);
                if (written > 0) {
                    sent += written;
                }
            }
            if (sent == header_bytes + p.size) {
                xQueueSend(free_, &slot, 0);
                transmitting = false;
            }
        }
        vTaskDelay(1);
    }
}
} // namespace usbchat
