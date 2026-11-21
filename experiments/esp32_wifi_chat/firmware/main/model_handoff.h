// Dedicated single-producer (API worker), single-consumer (owner) model result lane.
// One active Call and one owned packet. The producer retains unsuccessful submissions.
#pragma once
#include "protocol.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace wifichat {
inline bool matches_model_delivery(const Packet &packet, uint64_t call, uint32_t generation) {
    return (packet.kind == Kind::model_result || packet.kind == Kind::model_error) &&
           packet.id == call && (!packet.model.generation || packet.model.generation == generation);
}

class ModelHandoff {
  public:
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    void cancel() { generation_.fetch_add(1, std::memory_order_acq_rel); }

    bool post(const Packet &result, ModelDeliveryTiming timing) {
        if (timing.generation != generation() || full_.load(std::memory_order_acquire) ||
            result.size > tx_bytes ||
            (result.kind != Kind::model_result && result.kind != Kind::model_error)) {
            return false;
        }
        copy(result, packet_);
        packet_.model = timing;
        full_.store(true, std::memory_order_release);
        return true;
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
