#include "nvs_flash.h"
#include <cstring>
#include <string>
#include <vector>
namespace {
std::vector<uint8_t> blob;
std::string key;
} // namespace
esp_err_t nvs_flash_init() {
    return ESP_OK;
}
esp_err_t nvs_open(const char *, int, nvs_handle_t *out) {
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_blob(nvs_handle_t, const char *name, void *out, size_t *size) {
    if (key != name || blob.empty()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*size < blob.size()) {
        return ESP_FAIL;
    }
    std::memcpy(out, blob.data(), blob.size());
    *size = blob.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *name, const void *data, size_t size) {
    key = name;
    const auto *bytes = static_cast<const uint8_t *>(data);
    blob.assign(bytes, bytes + size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
void reset_nvs() {
    blob.clear();
    key.clear();
}
