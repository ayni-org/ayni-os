#include "../../../experiments/esp32_wifi_chat/firmware/wifi/model_delivery.h"
#include "speaker.h"
#include "transport.h"
#include "wake.h"
#include <cstring>
#include <deque>
#include <esp_timer.h>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

// Platform queues/notification and whole Display/Speaker endpoints are substituted.
// The real model completion, Transport packet pool, queue ordering and receive code are linked.
struct TestQueue {
    size_t capacity, item_size;
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> contents;
};
QueueHandle_t xQueueCreate(size_t capacity, size_t item_size) {
    static std::vector<std::unique_ptr<TestQueue>> queues;
    auto queue = std::make_unique<TestQueue>();
    queue->capacity = capacity;
    queue->item_size = item_size;
    queues.push_back(std::move(queue));
    return queues.back().get();
}
int xQueueSend(QueueHandle_t queue, const void *in, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->contents.size() == queue->capacity) {
        return 0;
    }
    auto bytes = static_cast<const uint8_t *>(in);
    queue->contents.emplace_back(bytes, bytes + queue->item_size);
    return pdTRUE;
}
int xQueuePeek(QueueHandle_t queue, void *out, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->contents.empty()) {
        return 0;
    }
    std::memcpy(out, queue->contents.front().data(), queue->item_size);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *out, TickType_t) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->contents.empty()) {
        return 0;
    }
    std::memcpy(out, queue->contents.front().data(), queue->item_size);
    queue->contents.pop_front();
    return pdTRUE;
}
namespace wifichat {
bool Display::initialize() {
    return true;
}
bool Display::submit_ui_frame(const uint8_t *, size_t) {
    return true;
}
Display::Completion display_test_completion = nullptr;
void *display_test_context = nullptr;
bool Display::submit_tool_frame(uint64_t, uint32_t, const uint8_t *, size_t, void *context,
                                Completion completion) {
    display_test_context = context;
    display_test_completion = completion;
    return true;
}
bool Speaker::receive(Kind, uint64_t, const uint8_t *, size_t) {
    return false;
}
AudioWrite Speaker::append_audio(uint64_t, uint32_t, const uint8_t *, size_t) {
    return AudioWrite::rejected;
}
namespace wifi {
void run(Transport &) {}
} // namespace wifi
} // namespace wifichat
