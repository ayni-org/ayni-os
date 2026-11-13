#include "motor_panel.h"
#include "app_menu.h"
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
    message_ = "已请求停止，重启后恢复";
}
bool MotorPanel::tick(Transport &transport, bool healthy, int64_t now) {
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
    return false;
}
MotorPanel::Tap MotorPanel::tap(Transport &transport, int x, int y, bool healthy) {
    if (x < 8 || x >= 232) {
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
    draw_motor_preferences(canvas, active(), limit_, message_);
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
