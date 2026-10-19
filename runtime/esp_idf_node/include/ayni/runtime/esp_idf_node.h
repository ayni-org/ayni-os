// Composition root for ESP-IDF targets (ADR-0010). M0 has only the Hello
// profile; the CORE_LITE profile with registration and ayni_start arrives in M2.

#ifndef AYNI_RUNTIME_ESP_IDF_NODE_H
#define AYNI_RUNTIME_ESP_IDF_NODE_H

#include <ayni/ayni.h>

namespace ayni::runtime::esp_idf_node {

// Hello profile: logs the platform baseline, creates the Kernel in internal
// RAM (ADR-0014), logs its lifecycle and the heap left afterwards, then
// destroys it and frees the memory. Returns AYNI_OK, AYNI_CAPACITY_EXCEEDED if
// internal RAM cannot hold the Kernel, or the first failing C API status. Call
// from one task; that task is the Kernel owner for the duration of the call.
ayni_status_t run_hello();

} // namespace ayni::runtime::esp_idf_node

#endif // AYNI_RUNTIME_ESP_IDF_NODE_H
