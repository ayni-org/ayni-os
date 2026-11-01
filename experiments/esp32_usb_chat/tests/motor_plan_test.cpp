#include "motor_plan.h"
#include <cassert>
int main() {
    using usbchat::motor_duties;
    // TA6586 manufacturer truth table p1: 00 coasts; never copy the module
    // picture's incorrect Motor1 "stop" row, which duplicates reverse drive.
    assert((motor_duties(0, 0) == std::array<uint32_t, 4>{0, 0, 0, 0}));
    assert((motor_duties(20, -35) == std::array<uint32_t, 4>{20, 0, 0, 35}));
    assert((motor_duties(-100, 100) == std::array<uint32_t, 4>{0, 100, 100, 0}));
    for (int32_t first = -usbchat::motor_percent_max; first <= usbchat::motor_percent_max;
         ++first) {
        const auto duty = motor_duties(first, -first);
        assert(duty[0] == duty[3] && duty[1] == duty[2]);
        assert(!duty[0] || !duty[1]);
        assert(!duty[2] || !duty[3]);
    }
}
