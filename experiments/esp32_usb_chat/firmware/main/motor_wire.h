#pragma once
#include "motor_plan.h"
#include "protocol.h"
namespace usbchat {
// Approval is ephemeral and tied to the actual Core attempt, never to model text.
struct MotorIdentity {
    uint64_t nonce = 0, call = 0, operation = 0, boot = 0, request = 0;
};
inline constexpr size_t motor_review_bytes = 64, motor_decision_bytes = 48;
inline void encode_motor_review(uint8_t *out, const MotorIdentity &id, const MotorCommand &command,
                                uint32_t limit, uint32_t phase) {
    put64(out, id.nonce);
    put64(out + 8, id.call);
    put64(out + 16, id.operation);
    put64(out + 24, id.boot);
    put64(out + 32, id.request);
    put32(out + 40, uint32_t(command.first_percent));
    put32(out + 44, uint32_t(command.second_percent));
    put32(out + 48, command.duration_ms);
    put32(out + 52, limit);
    put32(out + 56, phase);
    put32(out + 60, 0);
}
inline bool motor_decision_matches(const uint8_t *data, size_t bytes, const MotorIdentity &id,
                                   bool &approved) {
    if (bytes != motor_decision_bytes || get64(data) != id.nonce || get64(data + 8) != id.call ||
        get64(data + 16) != id.operation || get64(data + 24) != id.boot ||
        get64(data + 32) != id.request || get32(data + 40) > 1 || get32(data + 44) != 0) {
        return false;
    }
    approved = get32(data + 40) != 0;
    return true;
}
} // namespace usbchat
