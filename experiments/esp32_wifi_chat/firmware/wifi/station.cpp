#include "station.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
namespace wifichat::wifi {
namespace {
// Same three verification points as the previously accepted PS_NONE display
// profile. Logs expose policy metadata only, never credentials or AP details.
bool power_policy(const char *phase) {
    wifi_ps_type_t previous = WIFI_PS_MIN_MODEM, current = WIFI_PS_MIN_MODEM;
    const auto before = esp_wifi_get_ps(&previous);
    const auto set = esp_wifi_set_ps(WIFI_PS_NONE);
    const auto get = set == ESP_OK ? esp_wifi_get_ps(&current) : set;
    const bool verified = set == ESP_OK && get == ESP_OK && current == WIFI_PS_NONE;
    std::printf("wifi power_save phase=%s before=%d set=%ld get=%ld mode=%d verified=%u\n", phase,
                before == ESP_OK ? int(previous) : -1, long(set), long(get),
                get == ESP_OK ? int(current) : -1, unsigned(verified));
    return verified;
}
} // namespace
void Station::event(void *context, esp_event_base_t base, int32_t id, void *data) {
    auto &self = *static_cast<Station *>(context);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto &address = *static_cast<ip_event_got_ip_t *>(data);
        std::printf("wifi web=http://" IPSTR "\n", IP2STR(&address.ip_info.ip));
        self.link_.store(Link::policy_pending, std::memory_order_release);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        self.link_.store(Link::down, std::memory_order_release);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        self.scanned_.store(true, std::memory_order_release);
    }
}
bool Station::initialize() {
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK ||
        !esp_netif_create_default_wifi_sta()) {
        return false;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    if (esp_wifi_init(&config) != ESP_OK ||
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, this,
                                            &wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, this,
                                            &ip_handler_) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        return false;
    }
    if (!power_policy("after_start")) {
        return false;
    }
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    clock.start = false;
    clock.server_from_dhcp = false;
    if (esp_netif_sntp_init(&clock) != ESP_OK) {
        return false;
    }
    initialized_ = esp_netif_sntp_start() == ESP_OK;
    return initialized_;
}
bool Station::connect(const Settings &settings) {
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, settings.ssid.data(), std::strlen(settings.ssid.data()));
    std::memcpy(config.sta.password, settings.password.data(),
                std::strlen(settings.password.data()));
    config.sta.pmf_cfg.capable = true;
    return initialized_ && esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK &&
           power_policy("before_connect") && esp_wifi_connect() == ESP_OK;
}
bool Station::close_radio() {
    if (!release_radio()) {
        return false;
    }
    radio_ = RadioLease::closed;
    return true;
}
bool Station::release_radio() {
    if (radio_ == RadioLease::none || radio_ == RadioLease::closed) {
        return true;
    }
    if (radio_ == RadioLease::release_failed) {
        return false;
    } // Retain ownership, require reset.
    const auto status = esp_wifi_force_wakeup_release();
    std::printf("wifi rf_wake action=release status=%ld\n", long(status));
    radio_ = status == ESP_OK ? RadioLease::none : RadioLease::release_failed;
    return status == ESP_OK;
}
bool Station::poll() {
    if (radio_ == RadioLease::closed) {
        return true;
    }
    auto expected = Link::policy_pending;
    const auto link = link_.load(std::memory_order_acquire);
    if (link == Link::down) {
        return release_radio();
    }
    if (link != expected) {
        return true;
    }
    bool verified = power_policy("IP_READY");
    if (verified && radio_ == RadioLease::none) {
        // Candidate for observed idle brightness modulation: hold an explicit RF
        // reference in addition to PS_NONE. API acceptance is not a physical measurement.
        const auto status = esp_wifi_force_wakeup_acquire();
        std::printf("wifi rf_wake action=acquire status=%ld\n", long(status));
        if (status == ESP_OK) {
            radio_ = RadioLease::held;
        } else {
            verified = false;
        }
    }
    verified = verified && radio_ == RadioLease::held;
    if (link_.compare_exchange_strong(expected, verified ? Link::ready : Link::down,
                                      std::memory_order_acq_rel)) {
        if (!verified) {
            esp_wifi_disconnect();
            static_cast<void>(release_radio());
            return false;
        }
    } else if (expected == Link::down) {
        // A disconnect during acquire wins; its reference must not escape this turn.
        return release_radio();
    }
    return true;
}
bool Station::scan() {
    scanned_.store(false, std::memory_order_release);
    return initialized_ && power_policy("before_scan") &&
           esp_wifi_scan_start(nullptr, false) == ESP_OK;
}
bool Station::networks(std::array<wifi_ap_record_t, 8> &out, size_t &count) {
    if (!scanned_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    uint16_t size = out.size();
    if (esp_wifi_scan_get_ap_records(&size, out.data()) != ESP_OK) {
        return false;
    }
    count = size;
    return true;
}
} // namespace wifichat::wifi
