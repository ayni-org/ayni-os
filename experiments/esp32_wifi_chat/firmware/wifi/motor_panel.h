#pragma once
#include "../main/motor_plan.h"
#include "../main/transport.h"
#include "canvas.h"
#include <string>
namespace wifichat::wifi {
// UI-owned boot preparation and heartbeat. Core/PermitGate and the control
// worker own every actual action; the model does not supply this readiness.
class MotorPanel {
  public:
    enum class Tap { none, back };
    bool packet(const Packet &);
    bool tick(Transport &, bool healthy, int64_t now);
    Tap tap(Transport &, int x, int y, bool healthy);
    void draw(Canvas &) const;
    void stop(Transport &);
    void preparation_failed(Transport &transport) {
        stop(transport);
        message_ = "电机准备失败，须重启设备";
    }
    bool active() const { return wanted_ && !latched_; }

  private:
    bool wanted_ = false, armed_ = false, latched_ = false;
    uint32_t limit_ = motor_percent_max;
    int64_t heartbeat_ = 0;
    std::string message_ = "电机未准备";
};
} // namespace wifichat::wifi
