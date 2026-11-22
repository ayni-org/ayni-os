// Dedicated single-producer (API worker), single-consumer (owner) model result lane.
// One active Call and one owned packet. The producer retains unsuccessful submissions.
#pragma once
#include "protocol.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace wifichat {
enum class ModelPost { accepted, busy, stale, invalid };

inline bool matches_model_delivery(const Packet &packet, uint64_t call, uint32_t generation) {
    return (packet.kind == Kind::model_result || packet.kind == Kind::model_error) &&
           packet.id == call && (!packet.model.generation || packet.model.generation == generation);
}

inline ayni_provider_result_v1 model_delivery_failure(const Packet &packet) {
    if (packet.kind == Kind::model_error && packet.size == model_result_header_bytes) {
        return read_model_result_header(packet.body.data());
    }
    // A request whose terminal did not survive transport may already have been billed.
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = AYNI_CALL_ABANDONED;
    result.status = AYNI_RESULT_UNKNOWN;
    result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    return result;
}

class ModelHandoff {
  public:
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    void cancel() { generation_.fetch_add(1, std::memory_order_acq_rel); }

    ModelPost post(const Packet &result, ModelDeliveryTiming timing) {
        if (timing.generation != generation()) {
            return ModelPost::stale;
        }
        const bool text =
            result.kind == Kind::model_result && result.size > model_result_header_bytes;
        const bool error = result.kind == Kind::model_error &&
                           (result.size == 0 || result.size == model_result_header_bytes);
        if (!result.id || result.size > tx_bytes || (!text && !error)) {
            return ModelPost::invalid;
        }
        if (full_.load(std::memory_order_acquire)) {
            return ModelPost::busy;
        }
        copy(result, packet_);
        packet_.model = timing;
        full_.store(true, std::memory_order_release);
        return ModelPost::accepted;
    }
    bool receive(Packet &result) {
        if (!full_.load(std::memory_order_acquire)) {
            return false;
        }
        const bool current = packet_.model.generation == generation();
        if (current) {
            copy(packet_, result);
        }
        std::fill_n(packet_.body.data(), packet_.size, uint8_t{0});
        full_.store(false, std::memory_order_release);
        return current;
    }

  private:
    static void copy(const Packet &from, Packet &to) {
        to.kind = from.kind;
        to.id = from.id;
        to.size = from.size;
        to.model = from.model;
        std::memcpy(to.body.data(), from.body.data(), from.size);
    }
    Packet packet_{};
    std::atomic<bool> full_{false};
    std::atomic<uint32_t> generation_{1};
};
} // namespace wifichat
