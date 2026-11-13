#include "preferences.h"
#include <array>
#include <cstdint>
#include <nvs_flash.h>
namespace wifichat::wifi {
namespace {
constexpr uint8_t preference_version = 1;
constexpr size_t record_bytes = 2;
constexpr const char *preference_namespace = "ayni_ui", *voice_key = "voice_reply";
} // namespace
bool Preferences::load() {
    nvs_handle_t store = 0;
    if (nvs_open(preference_namespace, NVS_READWRITE, &store) != ESP_OK) {
        return false;
    }
    std::array<uint8_t, record_bytes> record{};
    size_t size = record.size();
    const auto rc = nvs_get_blob(store, voice_key, record.data(), &size);
    nvs_close(store);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        voice_reply_ = false;
        return true;
    }
    if (rc != ESP_OK || size != record.size() || record[0] != preference_version || record[1] > 1) {
        return false;
    }
    voice_reply_ = record[1] != 0;
    return true;
}
bool Preferences::set_voice_reply(bool enabled) {
    nvs_handle_t store = 0;
    if (nvs_open(preference_namespace, NVS_READWRITE, &store) != ESP_OK) {
        return false;
    }
    const std::array<uint8_t, record_bytes> record{preference_version, uint8_t(enabled)};
    auto rc = nvs_set_blob(store, voice_key, record.data(), record.size());
    if (rc == ESP_OK) {
        rc = nvs_commit(store);
    }
    nvs_close(store);
    if (rc != ESP_OK) {
        return false;
    }
    voice_reply_ = enabled;
    return true;
}
} // namespace wifichat::wifi
