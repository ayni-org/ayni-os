#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wifichat::wifi {
// Borrowed canonical root, shared by endpoint construction and route identity.
std::string_view normalized_api_root(std::string_view url);
// One boot's endpoint selection. Settings edits take effect after an explicit
// restart; an in-flight request can never change destination or credentials.
struct Settings {
    static constexpr size_t field_count = 6;
    std::array<char, 33> ssid{};
    std::array<char, 65> password{};
    std::array<char, 192> speech_url{};
    std::array<char, 192> model_url{};
    std::array<char, 192> model_key{};
    std::array<char, 64> model{};
    bool load();
    bool save() const;
    bool valid() const;
    bool network_valid() const;
    char *field(size_t index);
    size_t capacity(size_t index) const;
};
} // namespace wifichat::wifi
