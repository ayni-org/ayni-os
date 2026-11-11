#pragma once
#include <array>
#include <ayni/providers/model_protocol.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wifichat::wifi {
// Borrowed canonical root, shared by endpoint construction and route identity.
std::string_view normalized_api_root(std::string_view url);
// One boot's endpoint selection. Settings edits take effect after an explicit
// restart; an in-flight request can never change destination or credentials.
struct Settings {
    static constexpr size_t legacy_field_count = 6, field_count = 9;
    std::array<char, 33> ssid{};
    std::array<char, 65> password{};
    std::array<char, 192> speech_url{};
    std::array<char, 192> model_url{};
    std::array<char, 192> model_key{};
    std::array<char, 64> model{};
    std::array<char, 17> api_type{"chat_completions"};
    std::array<char, 6> stream{"false"};
    std::array<char, 22> output_limit{"max_tokens"};
    bool model_options_valid() const;
    // Explicit mapping of validated persisted options, shared by measure and transport.
    ayni::providers::model_protocol::Api model_api() const;
    ayni::providers::model_protocol::ChatTokenLimit model_token_limit() const;
    bool model_streaming() const;
    bool load();
    bool save() const;
    bool valid() const;
    bool network_valid() const;
    char *field(size_t index);
    size_t capacity(size_t index) const;
};
} // namespace wifichat::wifi
