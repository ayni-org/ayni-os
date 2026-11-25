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

// Only the platform queues/notification and unused hardware endpoints are substituted.
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
bool Display::submit_tool_frame(uint64_t, uint32_t, const uint8_t *, size_t, void *, Completion) {
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

namespace {
using namespace wifichat;
class ModelDelivery : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(transport.initialize());
        owner_task.store(&notifications);
    }
    void TearDown() override { owner_task.store(nullptr); }

    void deliver(Packet &packet, bool ok) {
        wifi::deliver_model_completion(
            transport,
            {packet, {}, {transport.api_generation(), 60, 75}, esp_timer_get_time() + 100000, ok});
    }
    ControlMailbox control;
    Transport transport{control};
    std::atomic<uint32_t> notifications{0};
};

TEST_F(ModelDelivery, OwnerReceivesWorkerCompletionBeforeBlockedFrameReleasesItsPackets) {
    std::promise<bool> full;
    std::promise<void> painted;
    auto release = painted.get_future();
    auto frame = std::async(std::launch::async, [&] {
        bool filled = true;
        for (size_t i = 0; i < output_packet_slots; ++i) {
            filled = transport.send(Kind::status, i, nullptr, 0) && filled;
        }
        full.set_value(filled);
        release.wait();
        Packet packet;
        return transport.take_output(packet);
    });
    EXPECT_TRUE(full.get_future().get());
    EXPECT_TRUE(transport.post(Kind::motor_heartbeat, 0, nullptr, 0));
    Packet heartbeat;
    ASSERT_TRUE(transport.receive(heartbeat));
    EXPECT_EQ(heartbeat.kind, Kind::motor_heartbeat);
    EXPECT_EQ(notifications.exchange(0), 1U);
    Packet result{}, received{};
    result.id = 41;
    result.kind = Kind::model_result;
    result.size = model_result_header_bytes + 1U;
    result.body[model_result_header_bytes] = 0x7f;
    auto worker = std::async(std::launch::async, [&] { deliver(result, true); });
    worker.get();
    const auto woken = notifications.exchange(0);
    const bool delivered = transport.receive(received);
    EXPECT_EQ(frame.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    painted.set_value();
    EXPECT_TRUE(frame.get());
    EXPECT_EQ(woken, 1U);
    ASSERT_TRUE(delivered);
    EXPECT_TRUE(matches_model_delivery(received, 41, transport.api_generation()));
    EXPECT_EQ(received.body[model_result_header_bytes], 0x7f);
    EXPECT_EQ(received.model.ready_us, 75U);
    EXPECT_FALSE(transport.receive(received));
}

TEST_F(ModelDelivery, UnclassifiedFailureWakesOwnerOnceAndWorkerCanDeliverNextCall) {
    Packet unknown{}, received{};
    unknown.id = 41;
    deliver(unknown, false); // No HTTP classification, exactly as a failed network completion.
    ASSERT_EQ(notifications.exchange(0), 1U);
    ASSERT_TRUE(transport.receive(received));
    ASSERT_TRUE(matches_model_delivery(received, 41, transport.api_generation()));
    EXPECT_EQ(received.kind, Kind::model_error);
    EXPECT_EQ(received.size, 0U);
    const auto failure = model_delivery_failure(received);
    EXPECT_EQ(failure.status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(failure.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(failure.effect, AYNI_CALL_EFFECT_UNKNOWN);
    EXPECT_FALSE(transport.receive(received));

    Packet next{};
    next.id = 42;
    next.kind = Kind::model_result;
    next.size = model_result_header_bytes + 1U;
    deliver(next, true);
    ASSERT_EQ(notifications.exchange(0), 1U);
    ASSERT_TRUE(transport.receive(received));
    EXPECT_TRUE(matches_model_delivery(received, 42, transport.api_generation()));
}
} // namespace
