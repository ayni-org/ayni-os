#include <ayni/runtime/esp_idf_node.h>

#include <ayni/hosts/esp_idf.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cinttypes>
#include <cstddef>

namespace ayni::runtime::esp_idf_node {
namespace {

constexpr const char *tag = "ayni.runtime";

// The Kernel stays in internal RAM: PSRAM is unavailable while the flash cache
// is disabled, and the stop chain must not depend on it (plan section 4.1).
constexpr uint32_t kernel_memory_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

ayni_status_t log_abi_version() {
    uint32_t major = 0;
    uint32_t minor = 0;
    const ayni_status_t status = ayni_get_abi_version(&major, &minor);
    if (status == AYNI_OK) {
        ESP_LOGI(tag, "Ayni ABI %" PRIu32 ".%" PRIu32 " (experimental), hello profile", major,
                 minor);
    }
    return status;
}

ayni_status_t log_kernel(const ayni_kernel_t *kernel, std::size_t size) {
    ayni_lifecycle_t lifecycle = 0;
    const ayni_status_t status = ayni_get_lifecycle(kernel, &lifecycle);
    if (status == AYNI_OK) {
        ESP_LOGI(tag, "kernel in internal RAM: %u bytes, lifecycle %" PRIu32 ", internal free %u",
                 static_cast<unsigned>(size), lifecycle,
                 static_cast<unsigned>(heap_caps_get_free_size(kernel_memory_caps)));
    }
    return status;
}

} // namespace

ayni_status_t run_hello() {
    hosts::esp_idf::log_platform_baseline();
    ayni_status_t status = log_abi_version();
    if (status != AYNI_OK) {
        return status;
    }

    ayni_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    config.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    config.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    config.agent_capacity = AYNI_AGENT_CAPACITY_DEFAULT;
    config.goal_type_capacity = AYNI_GOAL_TYPE_CAPACITY_DEFAULT;
    config.goal_capacity = AYNI_GOAL_CAPACITY_DEFAULT;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT;
    config.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_DEFAULT;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_DEFAULT;
    config.task_capacity = AYNI_TASK_CAPACITY_DEFAULT;
    std::size_t size = 0;
    std::size_t align = 0;
    status = ayni_kernel_memory_requirements(&config, &size, &align);
    if (status != AYNI_OK) {
        return status;
    }
    void *memory = heap_caps_aligned_alloc(align, size, kernel_memory_caps);
    if (memory == nullptr) {
        return AYNI_CAPACITY_EXCEEDED;
    }

    // Outlives the Kernel on every path that destroys it.
    hosts::esp_idf::Host platform;
    const ayni_host_v1 host = platform.table();
    ayni_kernel_t *kernel = nullptr;
    status = ayni_create(&config, &host, memory, size, &kernel);
    if (status != AYNI_OK) {
        heap_caps_free(memory);
        return status;
    }
    const ayni_status_t report_status = log_kernel(kernel, size);
    status = ayni_destroy(kernel);
    if (status != AYNI_OK) {
        // The Kernel still lives in this block, so it is deliberately not freed.
        return status;
    }
    heap_caps_free(memory);
    return report_status;
}

} // namespace ayni::runtime::esp_idf_node
