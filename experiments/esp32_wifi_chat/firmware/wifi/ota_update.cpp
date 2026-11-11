#include "ota_update.h"
#include "model_buffer.h"
#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace wifichat::wifi {
namespace {
constexpr size_t chunk_bytes = 4096;
constexpr size_t ota_data_bytes = 0x2000; // IDF OTA selection uses two independent 4 KiB sectors.
constexpr size_t descriptor_offset =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
constexpr size_t prefix_bytes = descriptor_offset + sizeof(esp_app_desc_t);
constexpr int64_t close_timeout_us = 30000000, upload_timeout_us = 180000000;
constexpr int64_t validation_timeout_us = 60000000, healthy_period_us = 5000000;
} // namespace
void OtaUpdate::initialize(DiagnosticLog &log) {
    log_ = &log;
    running_ = esp_ota_get_running_partition();
    const auto *first =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const auto *second =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const auto *data =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (running_ && first && second && data && data->size == ota_data_bytes &&
        (running_->address == first->address || running_->address == second->address)) {
        maximum_ = std::min(first->size, second->size);
        target_ = esp_ota_get_next_update_partition(running_);
        if (!target_ || target_->address == running_->address) {
            maximum_ = 0;
        }
    }
    esp_ota_img_states_t state{};
    if (running_ && esp_ota_get_state_partition(running_, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        verification_.store(Verification::pending);
        boot_deadline_ = esp_timer_get_time() + validation_timeout_us;
    }
    log.record("wifi ota initialized enabled=%u running=%lu maximum=%zu pending=%u\n",
               unsigned(maximum_ != 0),
               running_ ? static_cast<unsigned long>(running_->address) : 0UL, maximum_,
               unsigned(pending_verification()));
}
bool OtaUpdate::verify_boot(bool healthy) {
    if (verification_.load() != Verification::pending) {
        return false;
    }
    const auto now = esp_timer_get_time();
    if (now >= boot_deadline_) {
        verification_.store(Verification::failed);
        log_->record("wifi ota verification timed out; rollback requested\n");
        return true;
    }
    if (!healthy) {
        healthy_since_.reset();
        return false;
    }
    if (!healthy_since_) {
        healthy_since_ = now;
    }
    if (now - *healthy_since_ < healthy_period_us) {
        return false;
    }
    const auto result = esp_ota_mark_app_valid_cancel_rollback();
    verification_.store(result == ESP_OK ? Verification::confirmed : Verification::failed);
    log_->record("wifi ota verification status=%ld\n", long(result));
    return result != ESP_OK;
}
UpdatePlan::Snapshot OtaUpdate::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return plan_.snapshot();
}
bool OtaUpdate::begin_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    return plan_.close();
}
void OtaUpdate::stopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.stopped();
}
void OtaUpdate::close_refused(int error) {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.close_refused(error);
}
void OtaUpdate::fail(int error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plan_.fail(error);
    }
    log_->record("wifi ota failed status=%d\n", error);
}
void OtaUpdate::replied() {
    std::lock_guard<std::mutex> lock(mutex_);
    plan_.replied(uint64_t(esp_timer_get_time()) / 1000);
}
bool OtaUpdate::reboot_due() {
    std::lock_guard<std::mutex> lock(mutex_);
    return plan_.reboot_due(uint64_t(esp_timer_get_time()) / 1000);
}
bool OtaUpdate::progress(size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    return plan_.append(size);
}
esp_err_t OtaUpdate::status(httpd_req_t *request) {
    char body[1024];
    const auto state = snapshot();
    const auto *app = esp_app_get_description();
    ayni::json::Writer out(body, sizeof(body));
    out.raw("{\"version\":");
    out.string(std::string_view(app->version, strnlen(app->version, sizeof(app->version))));
    out.raw(",\"project\":");
    out.string(
        std::string_view(app->project_name, strnlen(app->project_name, sizeof(app->project_name))));
    out.raw(",\"running\":");
    out.string(running_ ? std::string_view(running_->label) : std::string_view{});
    out.raw(",\"ota_enabled\":");
    out.raw(maximum_ ? "true" : "false");
    out.raw(",\"pending_verify\":");
    out.raw(pending_verification() ? "true" : "false");
    out.raw(",\"max_bytes\":");
    out.unsigned_number(maximum_);
    out.raw(",\"phase\":");
    out.unsigned_number(uint32_t(state.phase));
    out.raw(",\"received\":");
    out.unsigned_number(state.received);
    out.raw(",\"total\":");
    out.unsigned_number(state.total);
    out.raw(",\"error\":");
    out.number(state.error);
    out.raw("}");
    size_t size = 0;
    if (!out.finish(size)) {
        return ESP_FAIL;
    }
    return httpd_resp_send(request, body, size);
}
bool OtaUpdate::valid_prefix(const uint8_t *bytes, size_t size) const {
    if (size < prefix_bytes) {
        return false;
    }
    esp_image_header_t header{};
    esp_app_desc_t incoming{};
    std::memcpy(&header, bytes, sizeof(header));
    std::memcpy(&incoming, bytes + descriptor_offset, sizeof(incoming));
    return header.magic == ESP_IMAGE_HEADER_MAGIC && header.hash_appended == 1 &&
           incoming.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           std::memcmp(incoming.project_name, esp_app_get_description()->project_name,
                       sizeof(incoming.project_name)) == 0;
}
esp_err_t OtaUpdate::receive(httpd_req_t *request, uint8_t *buffer, size_t capacity, size_t &got,
                             int64_t deadline) {
    while (esp_timer_get_time() < deadline) {
        const auto count = httpd_req_recv(request, reinterpret_cast<char *>(buffer), capacity);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (count <= 0) {
            return count == 0 ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
        }
        got = size_t(count);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
esp_err_t OtaUpdate::transfer(httpd_req_t *request, const uint8_t *prefix, size_t size,
                              size_t total) {
    esp_ota_handle_t handle = 0;
    auto result = esp_ota_begin(target_, total, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_ota_write(handle, prefix, size);
    if (result == ESP_OK && !progress(size)) {
        result = ESP_ERR_INVALID_STATE;
    }
    ModelBuffer buffer(chunk_bytes);
    if (!buffer.data && result == ESP_OK) {
        result = ESP_ERR_NO_MEM;
    }
    const auto deadline = esp_timer_get_time() + upload_timeout_us;
    size_t received = size;
    while (result == ESP_OK && received < total) {
        size_t got = 0;
        result =
            receive(request, buffer.data, std::min(buffer.size, total - received), got, deadline);
        if (result == ESP_OK) {
            result = esp_ota_write(handle, buffer.data, got);
        }
        if (result == ESP_OK) {
            received += got;
            if (!progress(got)) {
                result = ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (result != ESP_OK) {
        static_cast<void>(esp_ota_abort(handle));
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!plan_.verify()) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result != ESP_OK) {
        static_cast<void>(esp_ota_abort(handle));
        return result;
    }
    // esp_ota_end consumes the handle even on verification failure. Never select an unverified
    // image.
    result = esp_ota_end(handle);
    if (result == ESP_OK) {
        result = esp_ota_set_boot_partition(target_);
    }
    if (result == ESP_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!plan_.commit()) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    return result;
}
esp_err_t OtaUpdate::upload(httpd_req_t *request) {
    const size_t total = request->content_len;
    if (!maximum_ || pending_verification()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "OTA unavailable or startup not verified");
    }
    if (total < prefix_bytes || total > maximum_) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid application size");
    }
    ModelBuffer prefix(chunk_bytes);
    if (!prefix.data) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA scratch allocation failed");
    }
    size_t size = 0;
    const auto first_deadline = esp_timer_get_time() + close_timeout_us;
    while (size < prefix_bytes) {
        size_t got = 0;
        if (receive(request, prefix.data + size, std::min(prefix.size - size, total - size), got,
                    first_deadline) != ESP_OK) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                       "Incomplete application header");
        }
        size += got;
    }
    if (!valid_prefix(prefix.data, size)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Wrong project or invalid application header");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!plan_.request(total, maximum_)) {
            httpd_resp_set_status(request, "409 Conflict");
            return httpd_resp_sendstr(request, "Update already active");
        }
    }
    log_->record("wifi ota requested total=%zu\n", total);
    const auto deadline = esp_timer_get_time() + close_timeout_us;
    auto phase = snapshot().phase;
    while ((phase == UpdatePlan::Phase::requested || phase == UpdatePlan::Phase::closing) &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
        phase = snapshot().phase;
    }
    esp_err_t result = phase == UpdatePlan::Phase::receiving
                           ? transfer(request, prefix.data, size, total)
                           : ESP_ERR_INVALID_STATE;
    if (result != ESP_OK) {
        fail(result);
    }
    log_->record("wifi ota finished status=%ld received=%zu total=%zu\n", long(result),
                 snapshot().received, total);
    esp_err_t sent;
    if (result == ESP_OK) {
        sent = httpd_resp_sendstr(request, "{\"updated\":true,\"restarting\":true}");
    } else {
        httpd_resp_set_status(request, "503 Service Unavailable");
        sent = httpd_resp_sendstr(request, "Update not confirmed. Check firmware status and logs "
                                           "after restart; do not automatically retry.");
    }
    replied(); // A lost response never causes an automatic upload or image replay.
    return sent;
}
} // namespace wifichat::wifi
