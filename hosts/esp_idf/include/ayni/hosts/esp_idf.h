// ESP-IDF Host: the platform services the Kernel uses to maintain its own
// invariants (ADR-0006), plus the boot-time platform report required by
// MA-001 and plan section 4.2. M0 provides the clock and the report only.

#ifndef AYNI_HOSTS_ESP_IDF_H
#define AYNI_HOSTS_ESP_IDF_H

#include <ayni/ayni.h>

namespace ayni::hosts::esp_idf {

// Returns a service table that needs no context (ctx is null) and stays valid
// for the whole firmware run. The clock is esp_timer, which counts
// microseconds since boot and never decreases.
ayni_host_v1 host_table();

// Logs chip model and revision, ESP-IDF version, reset reason, detected flash
// and PSRAM sizes, and the internal/PSRAM/DMA heap baseline (total, free,
// minimum free, largest free block) plus the calling task's stack high-water
// mark. Reads state only; safe to call from any task after boot.
void log_platform_baseline();

} // namespace ayni::hosts::esp_idf

#endif // AYNI_HOSTS_ESP_IDF_H
