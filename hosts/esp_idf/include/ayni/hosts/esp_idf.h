// ESP-IDF Host: the platform services the Kernel uses to maintain its own
// invariants (ADR-0006): clock, wake and the ingress lock (ADR-0015), plus
// the boot-time platform report required by MA-001 and plan section 4.2.

#ifndef AYNI_HOSTS_ESP_IDF_H
#define AYNI_HOSTS_ESP_IDF_H

#include <ayni/ayni.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace ayni::hosts::esp_idf {

// Serves one Kernel. Construct it on the task that owns the Kernel: wake
// gives that task's default notification (index 0), which the owner's wait
// loop takes, so nothing else on that task may use index 0. The table's ctx
// points at this object, so it is neither copyable nor movable and must
// outlive the Kernel (C-HOST-001).
class Host {
  public:
    Host();
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // The clock is esp_timer, which counts microseconds since boot and never
    // decreases. wake and the ingress lock may be called from any task, never
    // from an ISR. The lock is a FreeRTOS mutex, so priority inheritance
    // bounds how long a low-priority poster can delay the owner task.
    ayni_host_v1 table();

  private:
    static void wake(void *ctx);
    static void ingress_lock(void *ctx);
    static void ingress_unlock(void *ctx);

    StaticSemaphore_t ingress_storage_;
    SemaphoreHandle_t ingress_;
    TaskHandle_t owner_;
};

// Logs chip model and revision, ESP-IDF version, reset reason, detected flash
// and PSRAM sizes, and the internal/PSRAM/DMA heap baseline (total, free,
// minimum free, largest free block) plus the calling task's stack high-water
// mark. Reads state only; safe to call from any task after boot.
void log_platform_baseline();

} // namespace ayni::hosts::esp_idf

#endif // AYNI_HOSTS_ESP_IDF_H
