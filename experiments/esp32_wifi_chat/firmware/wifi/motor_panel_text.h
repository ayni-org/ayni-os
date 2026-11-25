// Current UI activity takes precedence over a historical confirmed stop.
#pragma once
#include "../main/control_mailbox.h"
namespace wifichat::wifi {
inline const char *motor_output_text(const ControlFeedbackSnapshot &feedback) {
    if (feedback.stop_requested_seq > feedback.stop_confirmed_seq) {
        return "正在停机，等待硬件确认";
    }
    if (feedback.busy) {
        return feedback.hold_active ? "会话保持中" : "会话运行中";
    }
    if (!feedback.stop_confirmed_seq) {
        return "尚无停止确认";
    }
    return feedback.control_mode == uint8_t(ControlMode::armed) ? "输出已停止（软停，可恢复）"
                                                                : "输出已停止（急停锁存/未启用）";
}
} // namespace wifichat::wifi
