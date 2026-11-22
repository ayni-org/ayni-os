#include "transport.h"
#include "../wifi/runtime.h"
#include "speaker.h"
#include "wake.h"
#include <algorithm>
#include <cstring>
#include <esp_random.h>
#include <esp_timer.h>
namespace wifichat {
bool Transport::initialize() {
    diagnostics_.initialize(esp_random(), [] { return uint64_t(esp_timer_get_time()) / 1000; });
    free_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    output_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    input_ = xQueueCreate(packet_slots, sizeof(uint32_t));
    if (!free_ || !output_ || !input_ || !display_.initialize()) {
        return false;
    }
    for (uint32_t i = 0; i < packet_slots; ++i) {
        xQueueSend(free_, &i, 0);
    }
    return true;
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
    packet.size = static_cast<uint32_t>(bytes);
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
    // Older ordinary control keeps its ordering, but a best-effort outline may not
    // overtake the authoritative result that makes it eligible for presentation.
    if (xQueuePeek(input_, &slot, 0) != pdTRUE || packets_[slot].kind == Kind::tool_outline) {
        if (model_.receive(out)) {
            return true;
        }
    }
    if (xQueueReceive(input_, &slot, 0) != pdTRUE) {
        return model_.receive(out);
    }
    out = packets_[slot];
    xQueueSend(free_, &slot, 0);
    return true;
}
ModelPost Transport::post_model(const Packet &packet, ModelDeliveryTiming timing) {
    const auto posted = model_.post(packet, timing);
    if (posted == ModelPost::accepted) {
        wake_owner();
    }
    return posted;
}
bool Transport::take_output(Packet &out) {
    uint32_t slot = 0;
    if (xQueueReceive(output_, &slot, 0) != pdTRUE) {
        return false;
    }
    out = packets_[slot];
    xQueueSend(free_, &slot, 0);
    return true;
}
bool Transport::post(Kind kind, uint64_t id, const void *body, size_t bytes) {
    if (kind == Kind::motor_stop) {
        request_motor_stop();
        wake_owner();
        return true;
    }
    if ((kind == Kind::cancel && motor_active_.load(std::memory_order_acquire)) ||
        kind == Kind::shutdown || kind == Kind::motor_power_off) {
        request_motor_stop();
    }
    if (bytes > tx_bytes || (bytes && !body)) {
        return false;
    }
    if (kind == Kind::audio_begin || kind == Kind::audio_chunk || kind == Kind::audio_end ||
        kind == Kind::audio_error) {
        auto *speaker = speaker_.load(std::memory_order_acquire);
        return speaker && speaker->receive(kind, id, static_cast<const uint8_t *>(body), bytes);
    }
    uint32_t slot = 0;
    if (xQueueReceive(free_, &slot, 0) != pdTRUE) {
        return false;
    }
    auto &packet = packets_[slot];
    packet.kind = kind;
    packet.id = id;
    packet.size = static_cast<uint32_t>(bytes);
    if (bytes) {
        std::memcpy(packet.body.data(), body, bytes);
    }
    if (xQueueSend(input_, &slot, 0) != pdTRUE) {
        xQueueSend(free_, &slot, 0);
        return false;
    }
    wake_owner();
    return true;
}
AudioWrite Transport::append_audio(uint64_t call, uint32_t offset, const uint8_t *data,
                                   size_t bytes) {
    auto *speaker = speaker_.load(std::memory_order_acquire);
    return speaker ? speaker->append_audio(call, offset, data, bytes) : AudioWrite::rejected;
}
bool Transport::paint(uint64_t call, const uint8_t *pixels, size_t bytes) {
    return call && call == display_call_.load(std::memory_order_acquire) &&
           display_.paint(pixels, bytes);
}
void Transport::run() {
    wifi::run(*this);
}
} // namespace wifichat
