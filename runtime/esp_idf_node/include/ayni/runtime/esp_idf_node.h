// Board diagnostic composition entry. The full independent program is a separate consumer.
#ifndef AYNI_RUNTIME_ESP_IDF_NODE_H
#define AYNI_RUNTIME_ESP_IDF_NODE_H
#include <ayni/ayni.h>
namespace ayni::runtime::esp_idf_node {
// Owner task: report platform, ABI and default arena query only. The generic
// default arena no longer fits internal RAM; this entry makes no lifecycle claim.
// Returns the first public API failure. See tests/portability/esp32_s3 for the
// complete portable profile, PSRAM placement, asynchronous storage and ToolLoop.
ayni_status_t run_hello();
} // namespace ayni::runtime::esp_idf_node
#endif
