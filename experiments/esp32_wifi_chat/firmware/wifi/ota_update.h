#pragma once
#include "../main/diagnostic_log.h"
#include "update_plan.h"
#include <atomic>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <mutex>
namespace wifichat::wifi {
// Runtime owns this through HTTP-server lifetime. UI owns shutdown/boot verification;
// HTTP task alone owns the OTA handle and writes. The mutex protects only small metadata.
class OtaUpdate {
  public:
    void initialize(DiagnosticLog &);
    bool pending_verification() const {
        const auto state = verification_.load();
        return state == Verification::pending || state == Verification::failed;
    }
    // UI-only: request rollback if startup never reaches a stable healthy state.
    bool verify_boot(bool healthy);
    UpdatePlan::Snapshot snapshot();
    bool begin_close();
    void stopped();
    void close_refused(int error);
    bool reboot_due();
    esp_err_t status(httpd_req_t *);
    esp_err_t upload(httpd_req_t *);

  private:
    enum class Verification { none, pending, confirmed, failed };
    bool valid_prefix(const uint8_t *, size_t) const;
    esp_err_t transfer(httpd_req_t *, const uint8_t *, size_t, size_t);
    esp_err_t receive(httpd_req_t *, uint8_t *, size_t, size_t &, int64_t);
    bool progress(size_t);
    void fail(int);
    void replied();
    DiagnosticLog *log_ = nullptr; // Transport-owned, boot lifetime, metadata only.
    const esp_partition_t *running_ = nullptr, *target_ = nullptr;
    size_t maximum_ = 0; // Smallest OTA slot; zero disables updates on an unexpected layout.
    std::mutex mutex_;
    UpdatePlan plan_;
    std::atomic<Verification> verification_{Verification::none};
    int64_t boot_deadline_ = 0;
    std::optional<int64_t> healthy_since_; // UI-only, monotonic microseconds.
};
} // namespace wifichat::wifi
