#include "web_config.h"
#include <ayni/model_messages.hpp>
#include <cstdio>
#include <cstring>
#include <esp_netif.h>
#include <esp_timer.h>
#include <string>
#include <string_view>
#include <yyjson.h>
extern const char web_page[] asm("_binary_config_html_start");
namespace wifichat::wifi {
namespace {
constexpr size_t request_bytes = 2048;
constexpr const char *names[] = {"ssid",      "password",  "speech_url",
                                 "model_url", "model_key", "model"};
} // namespace
std::array<char, 40> WebConfig::address() {
    std::array<char, 40> result{};
    esp_netif_ip_info_t info{};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
        std::snprintf(result.data(), result.size(), "http://" IPSTR, IP2STR(&info.ip));
    }
    return result;
}
void WebConfig::headers(httpd_req_t *request, const char *type) {
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
bool WebConfig::same_origin(httpd_req_t *request) {
    char host[48]{}, origin[64]{};
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    const auto current = address();
    const char *numeric = current.data() + 7;
    const std::string_view authority(host);
    if (authority != numeric && authority != std::string(numeric) + ":80") {
        return false;
    }
    if (!httpd_req_get_hdr_value_len(request, "Origin")) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return false;
    }
    const std::string expected = std::string("http://") + host;
    return origin == expected;
}
bool WebConfig::initialize(const Settings &settings) {
    public_ = settings;
    password_set_ = *settings.password.data();
    key_set_ = *settings.model_key.data();
    public_.password.fill(0);
    public_.model_key.fill(0);
    patches_ = xQueueCreate(1, sizeof(Patch));
    results_ = xQueueCreate(2, sizeof(Result));
    if (!patches_ || !results_) {
        return false;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    if (httpd_start(&server_, &config) != ESP_OK) {
        return false;
    }
    httpd_uri_t routes[3]{};
    routes[0].uri = "/";
    routes[0].method = HTTP_GET;
    routes[0].handler = page;
    routes[1].uri = "/api/settings";
    routes[1].method = HTTP_GET;
    routes[1].handler = get;
    routes[2].uri = "/api/settings";
    routes[2].method = HTTP_POST;
    routes[2].handler = post;
    for (auto &route : routes) {
        route.user_ctx = this;
    }
    for (const auto &route : routes) {
        if (httpd_register_uri_handler(server_, &route) != ESP_OK) {
            return false;
        }
    }
    return true;
}
esp_err_t WebConfig::page(httpd_req_t *request) {
    if (!same_origin(request)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Use the device IP address");
    }
    headers(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, web_page, HTTPD_RESP_USE_STRLEN);
}
esp_err_t WebConfig::get(httpd_req_t *request) {
    if (!same_origin(request)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid origin");
    }
    auto &self = *static_cast<WebConfig *>(request->user_ctx);
    char body[2048]{};
    ayni::json::Writer out(body, sizeof(body));
    out.raw("{");
    for (size_t i = 0; i < Settings::field_count; ++i) {
        if (i == 1 || i == 4) {
            continue;
        }
        if (i) {
            out.raw(",");
        }
        out.string(std::string_view(names[i]));
        out.raw(":");
        const auto *value = self.public_.field(i);
        out.string(std::string_view(value));
    }
    out.raw(",\"password_set\":");
    out.raw(self.password_set_ ? "true" : "false");
    out.raw(",\"model_key_set\":");
    out.raw(self.key_set_ ? "true" : "false");
    out.raw("}");
    size_t size = 0;
    if (!out.finish(size)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Configuration unavailable");
    }
    headers(request, "application/json");
    return httpd_resp_send(request, body, size);
}
esp_err_t WebConfig::post(httpd_req_t *request) {
    auto &self = *static_cast<WebConfig *>(request->user_ctx);
    if (!same_origin(request)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid origin");
    }
    char type[64]{};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        std::strcmp(type, "application/json")) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Use application/json");
    }
    if (!self.editable_.load(std::memory_order_acquire)) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Cancel or finish the current turn before saving");
    }
    if (request->content_len <= 0 || request->content_len > request_bytes) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Request size invalid");
    }
    char body[request_bytes + 1]{};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Upload incomplete");
        }
        received += size_t(count);
    }
    Patch patch{};
    const bool valid = parse_settings_patch({body, received}, patch.change);
    volatile char *wipe = body;
    for (size_t i = 0; i < received; ++i) {
        wipe[i] = 0;
    }
    if (!valid) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid configuration fields");
    }
    const auto request_id = ++self.sequence_;
    patch.request_id = request_id;
    patch.deadline = esp_timer_get_time() + 3000000;
    const auto deadline = patch.deadline;
    const bool queued = xQueueSend(self.patches_, &patch, 0) == pdTRUE;
    patch.change.values.password.fill(0);
    patch.change.values.model_key.fill(0);
    Result result{};
    bool confirmed = false;
    while (queued && esp_timer_get_time() < deadline) {
        if (xQueueReceive(self.results_, &result, pdMS_TO_TICKS(50)) == pdTRUE &&
            result.request_id == request_id) {
            confirmed = true;
            break;
        }
    }
    if (!confirmed) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Save not confirmed; check device before retrying");
    }
    if (!result.saved) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Configuration not saved");
    }
    self.editable_.store(false, std::memory_order_release);
    headers(request, "application/json");
    return httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
}
bool WebConfig::take(Patch &patch) {
    return xQueueReceive(patches_, &patch, 0) == pdTRUE;
}
void WebConfig::complete(uint32_t request_id, bool saved) {
    const Result result{request_id, saved};
    if (xQueueSend(results_, &result, 0) != pdTRUE) {
        Result old{};
        xQueueReceive(results_, &old, 0);
        xQueueSend(results_, &result, 0);
    }
}
} // namespace wifichat::wifi
