#include "settings.h"
#include "../main/protocol.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <nvs_flash.h>

namespace wifichat::wifi {
std::string_view normalized_api_root(std::string_view url) {
    while (!url.empty() && url.back() == '/') {
        url.remove_suffix(1);
    }
    return url;
}
namespace {
constexpr Settings layout{};
constexpr size_t record_header = 8;
constexpr size_t record_bytes =
    record_header + layout.ssid.size() + layout.password.size() + layout.speech_url.size() +
    layout.model_url.size() + layout.model_key.size() + layout.model.size() +
    layout.api_type.size() + layout.stream.size() + layout.output_limit.size() + sizeof(uint32_t);
constexpr uint32_t record_magic = 0x41594331, record_version = 2;
} // namespace
char *Settings::field(size_t index) {
    switch (index) {
    case 0:
        return ssid.data();
    case 1:
        return password.data();
    case 2:
        return speech_url.data();
    case 3:
        return model_url.data();
    case 4:
        return model_key.data();
    case 5:
        return model.data();
    case 6:
        return api_type.data();
    case 7:
        return stream.data();
    default:
        return output_limit.data();
    }
}
size_t Settings::capacity(size_t index) const {
    const size_t sizes[] = {ssid.size(),      password.size(),  speech_url.size(),
                            model_url.size(), model_key.size(), model.size(),
                            api_type.size(),  stream.size(),    output_limit.size()};
    return index < std::size(sizes) ? sizes[index] : 0;
}
ayni::providers::model_protocol::Api Settings::model_api() const {
    return std::strcmp(api_type.data(), "responses") == 0
               ? ayni::providers::model_protocol::Api::responses
               : ayni::providers::model_protocol::Api::chat_completions;
}
ayni::providers::model_protocol::ChatTokenLimit Settings::model_token_limit() const {
    return std::strcmp(output_limit.data(), "max_completion_tokens") == 0
               ? ayni::providers::model_protocol::ChatTokenLimit::max_completion_tokens
               : ayni::providers::model_protocol::ChatTokenLimit::max_tokens;
}
bool Settings::model_streaming() const {
    return std::strcmp(stream.data(), "true") == 0;
}
bool Settings::model_options_valid() const {
    return (std::strcmp(api_type.data(), "chat_completions") == 0 ||
            std::strcmp(api_type.data(), "responses") == 0) &&
           (std::strcmp(stream.data(), "true") == 0 || std::strcmp(stream.data(), "false") == 0) &&
           (std::strcmp(output_limit.data(), "max_tokens") == 0 ||
            std::strcmp(output_limit.data(), "max_completion_tokens") == 0);
}
bool Settings::valid() const {
    // Plain HTTP speech is the user's LAN-only deployment choice. Never send
    // model credentials over it. URLs must not hide another authority/query.
    const auto clean = [](const char *s) {
        if (!*s || std::strpbrk(s, "\\ @?#")) {
            return false;
        }
        for (const auto *at = reinterpret_cast<const unsigned char *>(s); *at; ++at) {
            if (*at < 33 || *at == 127) {
                return false;
            }
        }
        const char *authority = std::strstr(s, "://");
        return authority && authority[3] && authority[3] != '/' && authority[3] != ':';
    };
    return model_options_valid() && network_valid() && clean(speech_url.data()) &&
           clean(model_url.data()) &&
           (std::strncmp(speech_url.data(), "http://", 7) == 0 ||
            std::strncmp(speech_url.data(), "https://", 8) == 0) &&
           std::strncmp(model_url.data(), "https://", 8) == 0 && *model_key.data() &&
           std::all_of(model_key.data(), model_key.data() + std::strlen(model_key.data()),
                       [](unsigned char c) { return c > 32 && c < 127; }) &&
           *model.data();
}
bool Settings::network_valid() const {
    const auto size = std::strlen(password.data());
    return *ssid.data() && (size == 0 || (size >= 8 && size <= 64));
}
bool Settings::load() {
    std::snprintf(model_url.data(), model_url.size(), "https://api.deepseek.com");
    std::snprintf(model.data(), model.size(), "deepseek-flash");
    // A separate new partition keeps legacy demo_cfg and all Ayni data intact.
    if (nvs_flash_init() != ESP_OK) {
        return false; // No erase-on-error fallback: an unknown partition stays intact.
    }
    nvs_handle_t store = 0;
    auto rc = nvs_open("ayni_wifi", NVS_READWRITE, &store);
    if (rc != ESP_OK) {
        return false;
    }
    std::array<uint8_t, record_bytes> record{};
    size_t size = record.size();
    rc = nvs_get_blob(store, "settings", record.data(), &size);
    nvs_close(store);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    const auto version = get32(record.data() + sizeof(uint32_t));
    if (rc != ESP_OK || (size != record.size() || version != record_version) ||
        get32(record.data()) != record_magic ||
        get32(record.data() + size - sizeof(uint32_t)) !=
            checksum(record.data(), size - sizeof(uint32_t))) {
        return false;
    }
    Settings candidate{};
    size_t at = record_header;
    for (size_t i = 0; i < field_count; ++i) {
        const auto bytes = capacity(i);
        if (!std::memchr(record.data() + at, 0, bytes)) {
            return false;
        }
        std::memcpy(candidate.field(i), record.data() + at, bytes);
        at += bytes;
    }
    if (!candidate.model_options_valid()) {
        return false;
    }
    *this = candidate;
    std::fill(record.begin(), record.end(), 0);
    return true;
}
bool Settings::save() const {
    // Wi-Fi can be provisioned first, before a browser supplies the model key.
    if (!network_valid() || !model_options_valid()) {
        return false;
    }
    nvs_handle_t store = 0;
    if (nvs_open("ayni_wifi", NVS_READWRITE, &store) != ESP_OK) {
        return false;
    }
    std::array<uint8_t, record_bytes> record{};
    put32(record.data(), record_magic);
    put32(record.data() + 4, record_version);
    const char *values[] = {ssid.data(),      password.data(),  speech_url.data(),
                            model_url.data(), model_key.data(), model.data(),
                            api_type.data(),  stream.data(),    output_limit.data()};
    size_t at = record_header;
    for (size_t i = 0; i < field_count; ++i) {
        std::memcpy(record.data() + at, values[i], std::strlen(values[i]) + 1);
        at += capacity(i);
    }
    put32(record.data() + at, checksum(record.data(), at));
    // NVS publishes a single blob. A sequence of nvs_set_str calls cannot be
    // rolled back by commit(), which is not a multi-key transaction in IDF.
    auto rc = nvs_set_blob(store, "settings", record.data(), record.size());
    if (rc == ESP_OK) {
        rc = nvs_commit(store);
    }
    nvs_close(store);
    std::fill(record.begin(), record.end(), 0);
    return rc == ESP_OK;
}
} // namespace wifichat::wifi
