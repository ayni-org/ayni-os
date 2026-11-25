#pragma once
#include "../main/motor_check.h"
#include "../main/motor_plan.h"
#include "../main/transport.h"
#include "canvas.h"
#include "motor_switch.h"
#include <string>
namespace wifichat::wifi {
// UI-owned desired enable state and heartbeat. Core/PermitGate and the control
// worker own every actual action; the model does not supply this readiness.
class MotorPanel {
  public:
    enum class Tap { none, test_first, test_second };
    bool packet(const Packet &);
    bool tick(Transport &, bool healthy, int64_t now);
    Tap tap(Transport &, int x, int y, bool healthy);
    void draw(Canvas &) const;
    void stop(Transport &);
    void initialized(bool ok) {
        control_.initialized(ok);
        message_ = ok ? "等待会话就绪" : "初始化失败，请重启检查";
    }
    void preparation_failed(Transport &transport, std::string_view detail) {
        stop(transport);
        const auto at = detail.find("status=");
        const auto end = at == detail.npos ? detail.npos : detail.find(';', at);
        message_ = "初始化失败 " +
                   std::string(at == detail.npos ? std::string_view("status=unknown")
                                                 : detail.substr(at, end - at)) +
                   "\n请重启后重试";
    }
    bool active() const { return control_.active(); }
    void test_notice(const MotorCheckNotice &);

  private:
    using State = MotorSwitch::State;
    MotorSwitch control_;
    uint32_t limit_ = motor_percent_max;
    int64_t heartbeat_ = 0;
    ControlFeedbackSnapshot feedback_{}; // UI-only independent receipt/application/stop frontiers.
    enum class Page { control, feedback, steering } page_ = Page::control;
    int32_t desired_first_ = 0, desired_second_ = 0;
    std::string feedback_text_;
    std::string message_ = "正在初始化电机资源";
};
} // namespace wifichat::wifi
