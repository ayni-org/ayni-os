#pragma once
#include "settings.h"
#include <array>
#include <atomic>
#include <esp_event.h>
#include <esp_wifi.h>
namespace wifichat::wifi {
// Wi-Fi callbacks publish facts only; UI owns connection commands and scans.
class Station {
  public:
    bool initialize();
    bool connect(const Settings &settings);
    bool close_radio(); // UI owner; forbids new RF references and releases before restart/close.
    bool poll();        // UI owner verifies the IP-ready power policy before publishing readiness.
    bool scan();
    bool networks(std::array<wifi_ap_record_t, 8> &out, size_t &count);
    bool connected() const { return link_.load(std::memory_order_acquire) == Link::ready; }

  private:
    enum class RadioLease { none, held, closed, release_failed };
    RadioLease radio_ =
        RadioLease::none;      // UI owner; exactly one successful acquire is paired with release.
    bool release_radio();      // Disconnect cleanup; does not close the boot-lifetime policy.
    bool initialized_ = false; // UI owner; connect/scan require the complete Wi-Fi policy setup.
    static void event(void *, esp_event_base_t, int32_t, void *);
    enum class Link { down, policy_pending, ready };
    std::atomic<Link> link_{
        Link::down}; // Callbacks publish pending/down; UI may confirm pending only.
    std::atomic<bool> scanned_{false};
    esp_event_handler_instance_t wifi_handler_ = nullptr, ip_handler_ = nullptr;
};
} // namespace wifichat::wifi
