#include "motor_panel.h"
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
        auto *enabled = yyjson_obj_get(yyjson_doc_get_root(doc), "motion_enabled");
        if (yyjson_is_bool(enabled)) {
            armed_ = wanted_ && !latched_ && yyjson_get_bool(enabled);
            message_ = armed_ ? "已准备，模型可直接操作" : "未准备或已禁用；停止后需重启";
        }
        yyjson_doc_free(doc);
    }
    return true;
}
void MotorPanel::stop(Transport &transport) {
    transport.request_motor_stop(); // Independent of command queue capacity.
    wanted_ = armed_ = false;
    latched_ = true;
    message_ = "已请求停止；重新准备须重启设备";
}
bool MotorPanel::tick(Transport &transport, bool healthy, int64_t now) {
    if (!healthy && active()) {
        stop(transport);
        return true;
    }
    if (active() && now - heartbeat_ >= heartbeat_us &&
        transport.post(Kind::motor_heartbeat, 0, nullptr, 0)) {
        heartbeat_ = now;
    }
    return false;
}
MotorPanel::Tap MotorPanel::tap(Transport &transport, int x, int y, bool healthy) {
    if (y >= 270) {
        if (x < 120) {
            return Tap::back;
        }
        stop(transport);
    } else if (y >= 190 && y < 232) {
        if (!healthy || latched_) {
            message_ = "会话未就绪或已停止；请重启后重新准备";
        } else {
            uint8_t bytes[sizeof(uint32_t)];
            put32(bytes, limit_);
            if (transport.post(Kind::motor_settings, 0, bytes, sizeof(bytes))) {
                wanted_ = true;
                message_ = "正在准备，电源状态未测量";
            } else {
                message_ = "准备队列已满，未准备";
            }
        }
    } else if (y >= 146 && y < 184 && !wanted_) {
        if (x < 120 && limit_ > 10) {
            limit_ -= 10;
        } else if (x >= 120 && limit_ < motor_percent_max) {
            limit_ += 10;
        }
    }
    return Tap::none;
}
void MotorPanel::draw(Canvas &canvas) const {
    canvas.label(8, 30, "电机 · 模型控制");
    canvas.document(message_, 0, 50, 88);
    canvas.label(8, 90, "先断电、架空，开关就手");
    canvas.label(8, 112, "准备后模型直接执行");
    canvas.label(8, 132, "单次上限60秒，PWM非转速");
    char label[64];
    std::snprintf(label, sizeof(label), "上限 %lu%%", static_cast<unsigned long>(limit_));
    canvas.button(0, 146, 116, 38, std::string("减小 · ") + label);
    canvas.button(122, 146, 116, 38, "增大上限");
    canvas.button(0, 190, 238, 42, "已断电、架空，准备电机");
    canvas.label(8, 238, "对话中直接要求操作电机");
    canvas.button(0, 270, 116, 44, "返回");
    canvas.button(122, 270, 116, 44, "停止");
}
} // namespace wifichat::wifi
