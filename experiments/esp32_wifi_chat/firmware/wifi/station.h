#pragma once
#include "../main/diagnostic_log.h"
#include "settings.h"
#include <array>
#include <atomic>
#include <esp_event.h>
#include <esp_wifi.h>
namespace wifichat::wifi {
// Wi-Fi callbacks publish facts only; UI owns connection commands and scans.
class Station {
  public:
    bool initialize(DiagnosticLog &);
    bool connect(const Settings &settings);
    bool close_radio(); // UI owner; forbids new RF references and releases before restart/close.
    bool poll();        // UI owner verifies the IP-ready power policy before publishing readiness.
    bool scan();
    bool networks(std::array<wifi_ap_record_t, 8> &out, size_t &count);
    bool connected() const { return link_.load(std::memory_order_acquire) == Link::ready; }

  private:
    enum class RadioLease { none, held, closed, release_failed };
    RadioLease radio_ =
        RadioLease::none; // UI owner; exactly one successful acquire is paired with release.
    bool release_radio(); // Disconnect cleanup; does not close the boot-lifetime policy.
    void poll_clock();    // UI owner; starts SNTP only after IP/power policy readiness.
    DiagnosticLog *diagnostics_ = nullptr; // Transport-owned, fixed metadata only.
    enum class Clock { idle, waiting, ready, failed };
    Clock clock_ = Clock::idle; // Reset on link loss; wall time remains owned by the system.
    int64_t next_clock_poll_us_ = 0, next_clock_log_us_ = 0;
    bool initialized_ = false; // UI owner; connect/scan require the complete Wi-Fi policy setup.
    static void event(void *, esp_event_base_t, int32_t, void *);
    enum class Link { down, policy_pending, ready };
    std::atomic<Link> link_{
        Link::down}; // Callbacks publish pending/down; UI may confirm pending only.
    std::atomic<bool> scanned_{false};
    esp_event_handler_instance_t wifi_handler_ = nullptr, ip_handler_ = nullptr;
};
} // namespace wifichat::wifi
