#include "app_menu.h"
#include "canvas.h"
#include <fstream>
#include <memory>
#include <string_view>
int main(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    auto canvas = std::make_unique<wifichat::wifi::Canvas>();
    canvas->clear();
    using namespace wifichat::wifi;
    canvas->label(92, 8, "Ayni", Canvas::muted);
    canvas->rounded_rectangle(154, 12, 6, 6, 3, Canvas::positive);
    const std::string_view mode(argv[2]);
    if (mode != "home") {
        canvas->button(4, 2, 48, 24, "返回");
    }
    if (mode == "motor") {
        draw_motor_preferences(*canvas, true, 90, "已启用");
    } else if (mode == "voice") {
        draw_voice_preferences(*canvas, true, "重启后保留");
    } else if (mode == "network") {
        const PreferenceRow rows[]{{"Wi-Fi 网络", "Home Wi-Fi"}, {"密码", "已设置"}};
        draw_endpoint_preferences(*canvas, "网络", rows, std::size(rows), false, false,
                                  "http://192.168.31.130");
    } else if (mode == "model") {
        const PreferenceRow rows[]{{"模型 API", "https://api.example.com"},
                                   {"API 密钥", "已设置"},
                                   {"模型名称", "example-model"},
                                   {"语音 API", "http://host:8000"}};
        draw_endpoint_preferences(*canvas, "模型与语音", rows, std::size(rows), false, true, {});
    } else if (mode == "options") {
        const PreferenceRow rows[]{{"接口", "Chat Completions"},
                                   {"流式预览", "开启"},
                                   {"Chat 输出字段", "max_completion_tokens"}};
        draw_endpoint_preferences(*canvas, "模型选项", rows, std::size(rows), true, false, {});
    } else {
        canvas->label(12, 28, "应用", Canvas::muted);
        draw_app_menu(*canvas, true);
    }
    canvas->button(180, 2, 56, 24, "停止", Canvas::danger, Canvas::background);
    std::ofstream output(argv[1], std::ios::binary);
    output << "P6\n240 320\n255\n";
    const auto *pixels = canvas->pixels();
    for (size_t at = 0; at < wifichat::screen_bytes; at += 2) {
        const auto color = uint16_t((uint16_t(pixels[at]) << 8) | pixels[at + 1]);
        const char rgb[]{char(((color >> 11) & 31) * 255 / 31),
                         char(((color >> 5) & 63) * 255 / 63), char((color & 31) * 255 / 31)};
        output.write(rgb, sizeof(rgb));
    }
    return output ? 0 : 1;
}
