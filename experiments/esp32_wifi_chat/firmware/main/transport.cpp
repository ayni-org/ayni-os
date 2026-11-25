#include "transport.h"
#include "../wifi/runtime.h"
#include "speaker.h"
#include "wake.h"
#include <algorithm>
#include <ayni/control_loop.h>
#include <cstring>
#include <esp_random.h>
#include <esp_timer.h>
namespace wifichat {
bool Transport::initialize() {
    diagnostics_.initialize(esp_random(), [] { return uint64_t(esp_timer_get_time()) / 1000; });
    input_free_ = xQueueCreate(input_packet_slots, sizeof(uint32_t));
    output_free_ = xQueueCreate(output_packet_slots, sizeof(uint32_t));
    output_ = xQueueCreate(output_packet_slots, sizeof(uint32_t));
    input_ = xQueueCreate(input_packet_slots, sizeof(uint32_t));
    if (!input_free_ || !output_free_ || !output_ || !input_ || !display_.initialize()) {
        return false;
    }
    for (uint32_t i = 0; i < input_packet_slots; ++i) {
        xQueueSend(input_free_, &i, 0);
    }
    for (uint32_t i = 0; i < output_packet_slots; ++i) {
        xQueueSend(output_free_, &i, 0);
    }
    return true;
}
bool Transport::send(Kind kind, uint64_t id, const void *body, size_t bytes) {
    if (bytes > summary_bytes || (bytes && !body)) {
        return false;
    }
    uint32_t slot = 0;
    if (xQueueReceive(output_free_, &slot, 0) != pdTRUE) {
        return false;
    }
    auto &packet = outputs_[slot];
    packet.kind = kind;
    packet.id = id;
    packet.size = static_cast<uint32_t>(bytes);
    if (bytes) {
        std::memcpy(packet.body.data(), body, bytes);
    }
    if (xQueueSend(output_, &slot, 0) == pdTRUE) {
        return true;
    }
    xQueueSend(output_free_, &slot, 0);
    return false;
}
bool Transport::send_latest_status(uint64_t id, const void *body, size_t bytes) {
    if (bytes > progress_bytes || (bytes && !body)) {
        return false;
    }
    return progress_.write(
        [&](auto &packet) {
            packet.kind = Kind::status;
            packet.id = id;
            packet.size = static_cast<uint32_t>(bytes);
            if (bytes) {
                std::memcpy(packet.body.data(), body, bytes);
            }
            return true;
        },
        [](const auto &, bool, const auto &) {});
}
bool Transport::receive(Packet &out) {
    uint32_t slot = 0;
    // Older ordinary control keeps its ordering, but a best-effort outline may not
    // overtake the authoritative result that makes it eligible for presentation.
    if (xQueuePeek(input_, &slot, 0) != pdTRUE || inputs_[slot].kind == Kind::tool_outline) {
        if (model_.receive(out)) {
            return true;
        }
    }
    if (xQueueReceive(input_, &slot, 0) != pdTRUE) {
        return model_.receive(out);
    }
    copy_packet(inputs_[slot], out);
    xQueueSend(input_free_, &slot, 0);
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
        BasicPacket<progress_bytes> latest;
        if (!progress_.read(latest)) {
            return false;
        }
        copy_packet(latest, out);
        return true;
    }
    copy_packet(outputs_[slot], out);
    xQueueSend(output_free_, &slot, 0);
    return true;
}
void Transport::request_motor_stop() {
    const auto at = uint64_t(esp_timer_get_time());
    const auto request = control_.request_stop(ControlStopKind::hard_estop, at);
    wake_owner();
    if (wake_motor_) {
        wake_motor_(motor_context_);
    }
    diagnostics_.record_deferred("wifi control_stop seq=%lu requested_at_us=%llu kind=hard_estop\n",
                                 (unsigned long)request.seq, (unsigned long long)at);
}
void Transport::request_motor_soft_stop() {
    const auto at = uint64_t(esp_timer_get_time());
    const auto request = control_.request_stop(ControlStopKind::soft_stop, at);
    wake_owner();
    if (wake_motor_) {
        wake_motor_(motor_context_);
    }
    diagnostics_.record_deferred("wifi control_stop seq=%lu requested_at_us=%llu kind=soft_stop\n",
                                 (unsigned long)request.seq, (unsigned long long)at);
}
bool Transport::submit_motor_target(ControlTarget target, uint64_t *seq) {
    const bool sent = control_.submit(target, uint64_t(esp_timer_get_time()), seq);
    if (sent && wake_motor_) {
        wake_motor_(motor_context_);
    }
    return sent;
}
bool Transport::request_motor_hold(uint32_t percent) {
    uint32_t seq = 0;
    const auto at = uint64_t(esp_timer_get_time());
    const bool sent = control_.request_hold(percent, &seq, at);
    if (sent && wake_motor_) {
        wake_motor_(motor_context_);
    }
    if (sent) {
        diagnostics_.record_deferred(
            "wifi control_hold seq=%lu accepted_at_us=%llu scale_pct=%lu\n", (unsigned long)seq,
            (unsigned long long)at, (unsigned long)percent);
    }
    return sent;
}
bool Transport::set_barge_in_policy(BargeInPolicy policy, uint32_t percent) {
    if (uint32_t(policy) > uint32_t(BargeInPolicy::continue_motion) ||
        percent > AYNI_CONTROL_LOOP_FULL_SCALE) {
        return false;
    }
    barge_in_.store((uint32_t(policy) << policy_shift) | percent, std::memory_order_release);
    return true;
}
bool Transport::apply_barge_in() {
    const auto settings = barge_in_.load(std::memory_order_acquire);
    const auto policy = BargeInPolicy(settings >> policy_shift);
    if (policy == BargeInPolicy::stop) {
        request_motor_soft_stop();
    } else {
        return request_motor_hold(policy == BargeInPolicy::hold ? settings & policy_mask
                                                                : AYNI_CONTROL_LOOP_FULL_SCALE);
    }
    return true;
}
bool Transport::post(Kind kind, uint64_t id, const void *body, size_t bytes) {
    if (bytes > tx_bytes || (bytes && !body)) {
        return false;
    }
    if (kind == Kind::motor_stop) {
        request_motor_stop();
        wake_owner();
        return true;
    }
    if (kind == Kind::shutdown || kind == Kind::motor_power_off) {
        request_motor_stop();
    } else if (kind == Kind::cancel && motor_active_.load(std::memory_order_acquire)) {
        // Ordinary UI cancel performs recoverable soft-stop; touchscreen red Stop uses
        // Kind::motor_stop / request_motor_stop() for latched hard E-stop (ADR-0111).
        request_motor_soft_stop();
    }
    if (kind == Kind::audio_begin || kind == Kind::audio_chunk || kind == Kind::audio_end ||
        kind == Kind::audio_error) {
        auto *speaker = speaker_.load(std::memory_order_acquire);
        return speaker && speaker->receive(kind, id, static_cast<const uint8_t *>(body), bytes);
    }
    if (bytes > input_bytes || (bytes && !body)) {
        return false;
    }
    uint32_t slot = 0;
    if (xQueueReceive(input_free_, &slot, 0) != pdTRUE) {
        return false;
    }
    auto &packet = inputs_[slot];
    packet.kind = kind;
    packet.id = id;
    packet.size = static_cast<uint32_t>(bytes);
    if (bytes) {
        std::memcpy(packet.body.data(), body, bytes);
    }
    // One policy boundary for every admitted natural-language producer. Reserve its input slot
    // before changing control policy, and publish the hold/stop before owner can read the input.
    if ((kind == Kind::user || kind == Kind::barge_in) &&
        motor_active_.load(std::memory_order_acquire)) {
        if (!apply_barge_in()) {
            xQueueSend(input_free_, &slot, 0);
            return false;
        }
    }
    if (xQueueSend(input_, &slot, 0) != pdTRUE) {
        xQueueSend(input_free_, &slot, 0);
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
bool Transport::paint(uint64_t call, const uint8_t *pixels, size_t bytes, uint32_t pages) {
    return call && call == display_call_.load(std::memory_order_acquire) &&
           display_.submit_tool_frame(
               call, pages, pixels, bytes, this,
               [](void *context, uint64_t id, uint32_t count, bool painted, uint8_t &stage) {
                   auto &transport = *static_cast<Transport *>(context);
                   if (!transport.display_allowed(id)) {
                       return true;
                   }
                   if (stage == 0) {
                       uint8_t body[sizeof(uint32_t)];
                       put32(body, count);
                       if (!transport.post(Kind::render_info, id, body, sizeof(body))) {
                           return false;
                       }
                       ++stage;
                   }
                   const uint8_t done = painted ? 1 : 0;
                   return transport.post(Kind::display_ack, id, &done, sizeof(done));
               });
}
void Transport::run() {
    wifi::run(*this);
}
} // namespace wifichat
