#pragma once
#include "settings.h"
#include <string_view>
namespace wifichat::wifi {
// Portable web-input contract. Omitted fields retain their saved values; empty
// password explicitly selects an open network. No saved secret is decoded here.
struct SettingsPatch {
    Settings values{};
    uint32_t fields = 0;
};
bool parse_settings_patch(std::string_view body, SettingsPatch &out);
} // namespace wifichat::wifi
