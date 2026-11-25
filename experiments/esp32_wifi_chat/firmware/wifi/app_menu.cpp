#include "app_menu.h"
#include "canvas.h"
#include <cstdio>
namespace wifichat::wifi {
namespace {
void icon(Canvas &c, App app, int x, int y) {
    constexpr uint16_t ink = Canvas::foreground;
    const auto line = [&](int a, int b, int d, int e) { c.line(x + a, y + b, x + d, y + e, ink); };
    switch (app) {
    case App::chat:
        c.rectangle(x + 8, y + 10, 30, 21, ink);
        c.rectangle(x + 11, y + 13, 24, 15, 0x2478);
        line(13, 31, 13, 36);
        line(13, 36, 20, 31);
        line(15, 18, 30, 18);
        line(15, 23, 26, 23);
        break;
    case App::voice:
        c.rectangle(x + 8, y + 19, 7, 12, ink);
        line(15, 19, 23, 12);
        line(23, 12, 23, 37);
        line(23, 37, 15, 30);
        line(29, 17, 34, 23);
        line(34, 23, 29, 32);
        line(35, 12, 41, 23);
        line(41, 23, 35, 37);
        break;
    case App::wifi:
        line(6, 17, 14, 11);
        line(14, 11, 32, 11);
        line(32, 11, 40, 17);
        line(12, 23, 18, 18);
        line(18, 18, 28, 18);
        line(28, 18, 34, 23);
        line(18, 29, 23, 25);
        line(23, 25, 28, 29);
        c.rectangle(x + 21, y + 34, 5, 5, ink);
        break;
    case App::model:
        c.rectangle(x + 13, y + 12, 23, 25, ink);
        c.rectangle(x + 16, y + 15, 17, 19, 0x5b1a);
        for (int at = 16; at <= 32; at += 8) {
            line(at, 7, at, 11);
            line(at, 38, at, 42);
            line(8, at - 1, 12, at - 1);
            line(37, at - 1, 41, at - 1);
        }
        line(21, 20, 21, 29);
        line(27, 20, 27, 29);
        line(21, 24, 27, 24);
        break;
    case App::remote:
        c.rectangle(x + 16, y + 6, 17, 37, ink);
        c.rectangle(x + 19, y + 9, 11, 31, 0xc36c);
        c.rectangle(x + 22, y + 13, 5, 5, ink);
        line(21, 25, 28, 25);
        line(24, 22, 24, 29);
        break;
    case App::motor:
        c.rectangle(x + 11, y + 15, 27, 22, ink);
        c.rectangle(x + 15, y + 19, 19, 14, 0xdea5);
        c.rectangle(x + 6, y + 21, 5, 10, ink);
        c.rectangle(x + 38, y + 23, 6, 5, ink);
        line(19, 15, 19, 9);
        line(19, 9, 30, 9);
        line(30, 9, 30, 15);
        break;
    case App::screen:
        c.rectangle(x + 6, y + 10, 36, 25, ink);
        c.rectangle(x + 9, y + 13, 30, 19, 0x2c32);
        line(24, 35, 24, 41);
        line(14, 42, 34, 42);
        break;
    case App::status:
        line(7, 35, 7, 22);
        line(8, 35, 8, 22);
        line(18, 35, 18, 14);
        line(19, 35, 19, 14);
        line(29, 35, 29, 7);
        line(30, 35, 30, 7);
        line(5, 40, 40, 40);
        break;
    case App::maintenance:
        line(13, 15, 35, 15);
        line(10, 11, 38, 11);
        line(20, 7, 29, 7);
        line(14, 16, 16, 39);
        line(16, 39, 32, 39);
        line(32, 39, 34, 16);
        line(21, 20, 22, 34);
        line(28, 20, 27, 34);
        break;
    }
}
} // namespace
void draw_app_menu(Canvas &canvas, bool voice_reply) {
    for (size_t at = 0; at < app_tiles.size(); ++at) {
        const auto &tile = app_tiles[at];
        const int x = 4 + int(at % app_columns) * app_pitch_x;
        const int y = app_top + int(at / app_columns) * app_pitch_y;
        canvas.rounded_rectangle(x + 12, y + 2, 48, 48, 10, tile.color);
        icon(canvas, tile.app, x + 12, y + 2);
        canvas.label(x + (app_tile_width - int(tile.name.size() / 3) * 16) / 2, y + 56, tile.name);
        if (tile.app == App::voice) {
            canvas.rectangle(x + 55, y + 3, 8, 8, voice_reply ? 0x07e0 : 0x8410);
        }
    }
}
void draw_voice_preferences(Canvas &canvas, bool enabled, std::string_view status) {
    canvas.heading("语音回复");
    canvas.rounded_rectangle(8, 84, 224, 76, 10, Canvas::surface);
    canvas.label(20, 96, "自动朗读");
    canvas.label(20, 128, enabled ? "已开启" : "已关闭",
                 enabled ? Canvas::positive : Canvas::muted);
    canvas.toggle(174, 98, enabled);
    canvas.label(16, 180, "朗读每轮最终答复", Canvas::muted);
    canvas.label(16, 204, status, Canvas::muted, 208);
}
void draw_motor_preferences(Canvas &canvas, bool enabled, uint32_t limit, std::string_view status) {
    canvas.heading("电机");
    canvas.rounded_rectangle(8, 62, 224, 80, 10, Canvas::surface);
    canvas.label(20, 74, "模型控制");
    canvas.toggle(174, 72, enabled);
    const auto split = status.find('\n');
    canvas.label(20, 100, status.substr(0, split), Canvas::muted, 200);
    if (split != status.npos) {
        canvas.label(20, 120, status.substr(split + 1), Canvas::muted, 200);
    }
    canvas.label(12, 154, "PWM 上限", Canvas::muted);
    canvas.label(148, 154, enabled ? "停用后调整" : "最长60秒", Canvas::muted);
    char label[64];
    std::snprintf(label, sizeof(label), "%lu%%", static_cast<unsigned long>(limit));
    canvas.button(8, 176, 44, 36, "-", Canvas::surface,
                  enabled ? Canvas::muted : Canvas::foreground);
    canvas.button(58, 176, 124, 36, label, Canvas::surface,
                  enabled ? Canvas::muted : Canvas::accent);
    canvas.button(188, 176, 44, 36, "+", Canvas::surface,
                  enabled ? Canvas::muted : Canvas::foreground);
    canvas.label(12, 224, "自检 · 50% / 5秒", Canvas::muted);
    canvas.button(8, 246, 108, 30, "测试 1");
    canvas.button(124, 246, 108, 30, "测试 2");
    canvas.button(8, 284, 224, 30, "停止", Canvas::danger, Canvas::background);
}
void draw_motor_feedback(Canvas &canvas, std::string_view status, std::string_view feedback) {
    canvas.heading("控制反馈");
    canvas.label(12, 62, status, Canvas::muted, 216);
    canvas.document(feedback, 0, 88, 276);
    canvas.button(8, 284, 224, 30, "停止", Canvas::danger, Canvas::background);
}
void draw_endpoint_preferences(Canvas &canvas, std::string_view title, const PreferenceRow *rows,
                               size_t count, bool options, bool show_options,
                               std::string_view web_address) {
    canvas.heading(title);
    for (size_t at = 0; at < count; ++at) {
        canvas.setting_row(preference_top + int(at) * (options ? option_pitch : preference_pitch),
                           options ? 52 : preference_height, rows[at].title, rows[at].value);
    }
    if (show_options) {
        canvas.button(8, 248, 224, 28, "接口与流式  >");
    } else if (!web_address.empty()) {
        canvas.label(12, 246, "Web 配置", Canvas::muted);
        canvas.label(12, 266, web_address, Canvas::muted, 216);
    }
    canvas.button(8, 284, 224, 30, "保存并重启", Canvas::accent, Canvas::background);
}
} // namespace wifichat::wifi
