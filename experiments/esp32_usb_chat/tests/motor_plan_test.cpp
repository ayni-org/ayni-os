#include "motor_contract.h"
#include "motor_wire.h"
#include <cassert>
int main() {
    using usbchat::motor_duties;
    const auto limits = usbchat::motor_limits();
    assert(limits.fields[0].minimum == -90 && limits.fields[0].maximum == 90);
    assert(limits.fields[1].minimum == -90 && limits.fields[1].maximum == 90);
    assert(limits.fields[2].minimum == 50 && limits.fields[2].maximum == 5000);
    // TA6586 manufacturer truth table p1: 00 coasts; never copy the module
    // picture's incorrect Motor1 "stop" row, which duplicates reverse drive.
    assert((motor_duties(0, 0) == std::array<uint32_t, 4>{0, 0, 0, 0}));
    assert((motor_duties(20, -35) == std::array<uint32_t, 4>{20, 0, 0, 35}));
    assert((motor_duties(-90, 90) == std::array<uint32_t, 4>{0, 90, 90, 0}));
    assert(usbchat::valid_motor_command({-90, 90, 5000}));
    assert(!usbchat::valid_motor_command({91, 0, 5000}));
    assert(!usbchat::valid_motor_command({0, -91, 5000}));
    assert(!usbchat::valid_motor_command({50, 0, 5001}));
    assert(!usbchat::valid_motor_command({50, 0, 49}));
    uint8_t data[usbchat::motor_review_bytes]{};
    const usbchat::MotorIdentity id{1, 2, 3, 4, 5};
    usbchat::encode_motor_review(data, id, {-90, 90, 5000}, 90, 1);
    usbchat::put32(data + 40, 1);
    usbchat::put32(data + 44, 0);
    bool approved = false;
    assert(usbchat::motor_decision_matches(data, usbchat::motor_decision_bytes, id, approved) &&
           approved);
    for (size_t at = 0; at < 40; at += sizeof(uint64_t)) {
        data[at] ^= 1;
        assert(!usbchat::motor_decision_matches(data, usbchat::motor_decision_bytes, id, approved));
        data[at] ^= 1;
    }
    usbchat::put32(data + 44, 1);
    assert(!usbchat::motor_decision_matches(data, usbchat::motor_decision_bytes, id, approved));
    for (int32_t first = -usbchat::motor_percent_max; first <= usbchat::motor_percent_max;
         ++first) {
        const auto duty = motor_duties(first, -first);
        assert(duty[0] == duty[3] && duty[1] == duty[2]);
        assert(!duty[0] || !duty[1]);
        assert(!duty[2] || !duty[3]);
    }
}
