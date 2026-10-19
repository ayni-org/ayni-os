// Firmware entry point. In M0 it runs the Hello profile once: platform
// baseline, empty Kernel create/destroy (MA-002). Motors are not driven.

#include <ayni/ayni.h>
#include <ayni/runtime/esp_idf_node.h>

#include <esp_log.h>

#include <cinttypes>

extern "C" void app_main(void) {
    const ayni_status_t status = ayni::runtime::esp_idf_node::run_hello();
    if (status == AYNI_OK) {
        ESP_LOGI("ayni", "hello profile finished");
    } else {
        ESP_LOGE("ayni", "hello profile failed with status %" PRId32, status);
    }
}
