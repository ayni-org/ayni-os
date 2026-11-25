#include "motor_panel.h"
#include "app_menu.h"
#include "motor_panel_text.h"
#include <algorithm>
#include <cstdio>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr int64_t heartbeat_us = 500000;
}
bool MotorPanel::packet(const Packet &packet) {
    if (packet.kind != Kind::motor_state || packet.size > 1024) {
        return false;
    }
    auto *doc = yyjson_read(reinterpret_cast<const char *>(packet.body.data()), packet.size, 0);
    if (doc) {
        auto *mode = yyjson_obj_get(yyjson_doc_get_root(doc), "control_mode");
        if (yyjson_is_uint(mode)) {
            const auto value = yyjson_get_uint(mode);
            control_.observe(uint32_t(value));
            message_ = control_.state() == State::enabled     ? "已启用"
                       : control_.state() == State::disabling ? "正在停用"
                       : control_.state() == State::disabled  ? "电机已停用"
                                                              : "已停止，重启后恢复";
        }
        yyjson_doc_free(doc);
    }
    return true;
}
void MotorPanel::stop(Transport &transport) {
    transport.request_motor_stop(); // Independent of command queue capacity.
    control_.stop();
    message_ = "正在停机，等待硬件确认";
}
bool MotorPanel::tick(Transport &transport, bool healthy, int64_t now) {
    const auto feedback = transport.control_mailbox().snapshot();
    const bool changed = feedback.accepted_seq != feedback_.accepted_seq ||
                         feedback.superseded_count != feedback_.superseded_count ||
                         feedback.applied_seq != feedback_.applied_seq ||
                         feedback.rejected_seq != feedback_.rejected_seq ||
                         feedback.stop_confirmed_seq != feedback_.stop_confirmed_seq ||
                         feedback.stop_requested_seq != feedback_.stop_requested_seq ||
                         feedback.control_mode != feedback_.control_mode ||
                         feedback.busy != feedback_.busy ||
                         feedback.hold_active != feedback_.hold_active ||
                         feedback.applied_motor1 != feedback_.applied_motor1 ||
                         feedback.applied_motor2 != feedback_.applied_motor2;
    if (changed) {
        feedback_ = feedback;
        control_.observe(feedback.control_mode);
        const char *reason =
            feedback.reject_reason == ControlRejectReason::no_active_session ? "无活跃许可会话"
            : feedback.reject_reason == ControlRejectReason::out_of_envelope ? "超出许可包络"
            : feedback.reject_reason == ControlRejectReason::stop_pending ? "停止信号已拦截"
            : feedback.reject_reason == ControlRejectReason::disarmed_or_latched
                ? "未启用或急停锁存"
            : feedback.reject_reason == ControlRejectReason::interlock_fault ? "联锁或传感失效"
                                                                             : "无";
        char text[384];
        std::snprintf(
            text, sizeof(text),
            "已接收 #%llu\n已被覆盖 #%llu（%llu次）\n已应用 #%llu M1:%ld M2:%ld%%\n已拒绝 #%llu "
            "%s\n%s",
            (unsigned long long)feedback.accepted_seq, (unsigned long long)feedback.superseded_seq,
            (unsigned long long)feedback.superseded_count, (unsigned long long)feedback.applied_seq,
            long(feedback.applied_motor1), long(feedback.applied_motor2),
            (unsigned long long)feedback.rejected_seq, reason, motor_output_text(feedback));
        feedback_text_ = text;
    }
    if (!healthy && active()) {
        stop(transport);
        return true;
    }
    if (healthy && control_.desired() && control_.state() == State::disabled) {
        uint8_t bytes[sizeof(uint32_t)];
        put32(bytes, limit_);
        if (transport.post(Kind::motor_settings, 0, bytes, sizeof(bytes))) {
            transport.set_motor_active(true);
            control_.queued();
            message_ = "正在启用";
        }
    }
    if (active() && now - heartbeat_ >= heartbeat_us &&
        transport.post(Kind::motor_heartbeat, 0, nullptr, 0)) {
        heartbeat_ = now;
    }
    return changed;
}
MotorPanel::Tap MotorPanel::tap(Transport &transport, int x, int y, bool healthy) {
    if (x < 8 || x >= 232) {
        return Tap::none;
    }
    if (x >= 150 && y >= 32 && y < 58) {
        page_ = page_ == Page::control    ? Page::feedback
                : page_ == Page::feedback ? Page::steering
                                          : Page::control;
        return Tap::none;
    }
    if (page_ == Page::steering && ((y >= 100 && y < 144) || (y >= 166 && y < 210))) {
        if (!healthy || !active()) {
            message_ = "请先启用并打开许可会话";
            return Tap::none;
        }
        auto &desired = y < 160 ? desired_first_ : desired_second_;
        desired = x < 80     ? std::max(desired - 10, -int32_t(limit_))
                  : x >= 160 ? std::min(desired + 10, int32_t(limit_))
                             : 0;
        ControlTarget target;
        target.motor1 = desired_first_;
        target.motor2 = desired_second_;
        target.duration_ms = 500;
        message_ =
            transport.submit_motor_target(target) ? "已受理，应用结果见反馈" : "邮箱忙，请重试";
        return Tap::none;
    }
    if (page_ != Page::control && !(y >= 284 && y < 314)) {
        return Tap::none;
    }
    if (y >= 284 && y < 314) {
        stop(transport);
    } else if (y >= 246 && y < 276) {
        if (x >= 116 && x < 124) {
            return Tap::none;
        }
        if (!healthy || control_.state() != State::enabled) {
            message_ = "未启用或当前忙碌";
        } else if (limit_ < MotorCheck::percent) {
            message_ = "PWM上限低于50%，未执行";
        } else {
            return x < 120 ? Tap::test_first : Tap::test_second;
        }
    } else if (y >= 62 && y < 142) {
        if (control_.active()) {
            control_.disable();
            transport.set_motor_active(false);
            transport.request_motor_disable();
            message_ = "正在停用";
        } else if (healthy && control_.state() == State::disabled) {
            control_.enable();
            message_ = "等待启用";
        } else {
            message_ = "会话未就绪或已停止锁定";
        }
    } else if (y >= 176 && y < 212 && control_.state() == State::disabled) {
        if (x >= 8 && x < 52 && limit_ > 10) {
            limit_ -= 10;
        } else if (x >= 188 && x < 232 && limit_ < motor_percent_max) {
            limit_ += 10;
        }
    }
    return Tap::none;
}
void MotorPanel::draw(Canvas &canvas) const {
    if (page_ == Page::steering) {
        draw_motor_steering(canvas, desired_first_, desired_second_, message_);
    } else if (page_ == Page::feedback) {
        draw_motor_feedback(canvas, message_,
                            feedback_text_.empty() ? "等待控制指令" : feedback_text_);
    } else {
        draw_motor_preferences(canvas, active(), limit_, message_);
    }
    canvas.button(150, 32, 80, 26,
                  page_ == Page::control    ? "反馈"
                  : page_ == Page::feedback ? "调速"
                                            : "控制");
}
void MotorPanel::test_notice(const MotorCheckNotice &notice) {
    char text[160];
    const char *phase = notice.phase == MotorCheckNotice::Phase::queued    ? "等待执行"
                        : notice.phase == MotorCheckNotice::Phase::running ? "自检执行中"
                        : notice.phase == MotorCheckNotice::Phase::completed
                            ? "测试完成，输出已停止"
                        : notice.phase == MotorCheckNotice::Phase::cancelled ? "测试已取消"
                                                                             : "自检失败";
    std::snprintf(text, sizeof(text), "电机%lu %s\nstatus=%ld reason=%lu",
                  static_cast<unsigned long>(notice.motor + 1), phase, long(notice.status),
                  static_cast<unsigned long>(notice.reason));
    message_ = text;
}
} // namespace wifichat::wifi
