#include "nvs_flash.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace {
std::map<std::pair<std::string, std::string>, std::vector<uint8_t>> blobs;
std::map<nvs_handle_t, std::string> handles;
nvs_handle_t next_handle = 0;
} // namespace
esp_err_t nvs_flash_init() {
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int, nvs_handle_t *out) {
    *out = ++next_handle;
    handles[*out] = name;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    handles.erase(handle);
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *name, void *out, size_t *size) {
    const auto found = blobs.find({handles.at(handle), name});
    if (found == blobs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const auto &blob = found->second;
    if (*size < blob.size()) {
        return ESP_FAIL;
    }
    std::memcpy(out, blob.data(), blob.size());
    *size = blob.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *name, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    blobs[{handles.at(handle), name}].assign(bytes, bytes + size);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
void reset_nvs() {
    blobs.clear();
    handles.clear();
    next_handle = 0;
}
