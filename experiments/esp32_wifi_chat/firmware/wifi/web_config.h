#pragma once
#include "settings_codec.h"
#include <atomic>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
namespace wifichat::wifi {
// User-selected unauthenticated LAN configuration. HTTP only owns a bounded
// proposed patch; the UI owner saves it and requests orderly Core shutdown.
// Saved secrets never appear in GET responses, HTML, errors or logs.
class WebConfig {
  public:
    struct Patch {
        SettingsPatch change{};
        uint32_t request_id = 0;
        int64_t deadline = 0; // Monotonic microseconds; owner refuses an expired proposal.
    };
    bool initialize(const Settings &settings);
    void editable(bool value) { editable_.store(value, std::memory_order_release); }
    bool take(Patch &patch);
    void complete(uint32_t request_id, bool saved);
    static std::array<char, 40> address();

  private:
    static esp_err_t page(httpd_req_t *);
    static esp_err_t get(httpd_req_t *);
    static esp_err_t post(httpd_req_t *);
    static bool same_origin(httpd_req_t *);
    static void headers(httpd_req_t *, const char *type);
    Settings public_{}; // Password/key arrays always zero; only booleans below escape.
    bool password_set_ = false, key_set_ = false;
    httpd_handle_t server_ = nullptr;
    QueueHandle_t patches_ = nullptr, results_ = nullptr;
    struct Result {
        uint32_t request_id = 0;
        bool saved = false;
    };
    uint32_t sequence_ = 0; // HTTP-server task only; no concurrent handlers.
    std::atomic<bool> editable_{true};
};
} // namespace wifichat::wifi
