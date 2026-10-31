// Board diagnostic entry; the actual independent program lives under tests/portability.
#include <ayni/hosts/esp_idf.h>
#include <ayni/runtime/common/default_config.h>
#include <ayni/runtime/esp_idf_node.h>
#include <cinttypes>
#include <esp_log.h>
namespace ayni::runtime::esp_idf_node {
ayni_status_t run_hello() {
    hosts::esp_idf::log_platform_baseline();
    uint32_t major = 0, minor = 0;
    auto status = ayni_get_abi_version(&major, &minor);
    const auto config = default_config();
    size_t bytes = 0, alignment = 0;
    if (status == AYNI_OK) {
        status = ayni_kernel_memory_requirements(&config, &bytes, &alignment);
    }
    if (status == AYNI_OK) {
        ESP_LOGI("ayni.diagnostic",
                 "ABI %" PRIu32 ".%" PRIu32 "; default arena query %u align %u; no Kernel started",
                 major, minor, static_cast<unsigned>(bytes), static_cast<unsigned>(alignment));
    }
    return status;
}
} // namespace ayni::runtime::esp_idf_node
