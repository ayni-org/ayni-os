#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
namespace wifichat::wifi {
class Canvas;
enum class App { chat, voice, wifi, model, remote, motor, screen, status, maintenance };
struct AppTile {
    App app;
    std::string_view name;
    uint16_t color;
};
inline constexpr std::array<AppTile, 9> app_tiles{{{App::chat, "聊天", 0x2478},
                                                   {App::voice, "语音回复", 0x8adf},
                                                   {App::wifi, "网络", 0x04d4},
                                                   {App::model, "模型", 0x5b1a},
                                                   {App::remote, "遥控器", 0xc36c},
                                                   {App::motor, "电机", 0xdea5},
                                                   {App::screen, "屏幕", 0x2c32},
                                                   {App::status, "状态", 0x5b8e},
                                                   {App::maintenance, "清理", 0xa42a}}};
inline constexpr int app_columns = 3, app_pitch_x = 80, app_pitch_y = 82, app_top = 46;
inline constexpr int app_tile_width = 72, app_tile_height = 78;
inline std::optional<App> app_at(int x, int y) {
    if (x < 4 || y < app_top) {
        return {};
    }
    const auto column = (x - 4) / app_pitch_x, row = (y - app_top) / app_pitch_y;
    const auto index = row * app_columns + column;
    if (column >= app_columns || index < 0 || size_t(index) >= app_tiles.size() ||
        (x - 4) % app_pitch_x >= app_tile_width || (y - app_top) % app_pitch_y >= app_tile_height) {
        return {};
    }
    return app_tiles[size_t(index)].app;
}
void draw_app_menu(Canvas &, bool voice_reply);
void draw_motor_preferences(Canvas &, bool enabled, uint32_t limit, std::string_view status);
void draw_voice_preferences(Canvas &, bool enabled, std::string_view status);
struct PreferenceRow {
    std::string_view title;
    std::string_view value;
};
inline constexpr int preference_top = 62, preference_pitch = 46, preference_height = 42;
inline constexpr int option_pitch = 58;
void draw_endpoint_preferences(Canvas &, std::string_view title, const PreferenceRow *,
                               size_t count, bool options, bool show_options,
                               std::string_view web_address);
} // namespace wifichat::wifi
