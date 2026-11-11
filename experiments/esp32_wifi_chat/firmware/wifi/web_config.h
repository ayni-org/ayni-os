#pragma once
#include "../main/diagnostic_log.h"
#include "settings_codec.h"
#include <atomic>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
namespace wifichat::wifi {
class OtaUpdate;
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
    bool initialize(const Settings &settings, DiagnosticLog &diagnostics, OtaUpdate &update);
    void editable(bool value) { editable_.store(value, std::memory_order_release); }
    bool take(Patch &patch);
    void complete(uint32_t request_id, bool saved);
    uint32_t take_probe() { return probe_requested_.exchange(0, std::memory_order_acq_rel); }
    static std::array<char, 40> address();

  private:
    static esp_err_t page(httpd_req_t *);
    static esp_err_t get(httpd_req_t *);
    static esp_err_t post(httpd_req_t *);
    static esp_err_t logs(httpd_req_t *);
    static esp_err_t logs_page(httpd_req_t *);
    static esp_err_t probe(httpd_req_t *);
    static esp_err_t firmware_page(httpd_req_t *);
    static esp_err_t firmware_get(httpd_req_t *);
    static esp_err_t firmware_post(httpd_req_t *);
    static bool same_origin(httpd_req_t *);
    static void headers(httpd_req_t *, const char *type);
    Settings public_{}; // Password/key arrays always zero; only booleans below escape.
    DiagnosticLog *diagnostics_ = nullptr; // Transport-owned, boot lifetime; metadata only.
    OtaUpdate *update_ = nullptr;          // Runtime-owned; HTTP borrows through boot lifetime.
    bool password_set_ = false, key_set_ = false;
    httpd_handle_t server_ = nullptr;
    QueueHandle_t patches_ = nullptr, results_ = nullptr;
    struct Result {
        uint32_t request_id = 0;
        bool saved = false;
    };
    uint32_t sequence_ = 0; // HTTP-server task only; no concurrent handlers.
    std::atomic<bool> editable_{true};
    // Zero: none; UINT32_MAX: transport probe; otherwise a complete synthetic Han history turn.
    std::atomic<uint32_t> probe_requested_{0};
};
} // namespace wifichat::wifi
