// Bounded ingress queue (MA-003; C-LIMIT-001, C-LIMIT-008, ADR-0015):
// ordering, the full-queue policy for droppable and other events, and
// concurrent producers under the Host lock.

#include "kernel/ingress/event_queue.h"
#include "kernel/ingress/ingress_lock.h"

#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace {

using ayni::kernel::EventHeader;
using ayni::kernel::EventQueue;

constexpr uint32_t control_kind = 3;   // a local input: never dropped
constexpr uint32_t telemetry_kind = 2; // droppable

EventHeader control_event(uint64_t sequence) {
    EventHeader header{};
    header.kind = control_kind;
    header.sequence = sequence;
    header.body_size = sizeof(sequence);
    return header;
}

EventHeader telemetry(uint64_t subscription, uint32_t key, uint64_t sequence) {
    EventHeader header{};
    header.kind = telemetry_kind;
    header.subscription_id = subscription;
    header.coalesce_key = key;
    header.sequence = sequence;
    header.droppable = true;
    header.body_size = sizeof(sequence);
    return header;
}

// Each event carries its sequence as the body, so a test can check that
// header and body still belong together after replacement.
ayni_status_t push(EventQueue &queue, const EventHeader &header) {
    uint8_t body[sizeof(uint64_t)];
    std::memcpy(body, &header.sequence, sizeof(header.sequence));
    return queue.push(header, body);
}

struct Popped {
    EventHeader header;
    uint64_t body_sequence;
};

std::vector<Popped> pop_all(EventQueue &queue) {
    std::vector<Popped> popped;
    EventHeader header{};
    std::array<uint8_t, 64> body{};
    while (queue.pop(header, body.data())) {
        uint64_t sequence = 0;
        std::memcpy(&sequence, body.data(), sizeof(sequence));
        popped.push_back({header, sequence});
    }
    return popped;
}

struct QueueStorage {
    QueueStorage(uint32_t capacity, uint32_t body_max)
        : bytes(EventQueue::storage_size(capacity, body_max) / sizeof(uint64_t) + 1U),
          queue(bytes.data(), capacity, body_max) {}
    std::vector<uint64_t> bytes; // uint64_t elements satisfy storage_alignment
    EventQueue queue;
};

TEST(IngressQueue, ten_thousand_events_pass_in_order_without_loss) {
    QueueStorage storage(AYNI_INGRESS_CAPACITY_DEFAULT, 16);
    EventQueue &queue = storage.queue;
    std::vector<Popped> delivered;
    uint64_t busy = 0;
    for (uint64_t sequence = 0; sequence < 10000U;) {
        const ayni_status_t status = push(queue, control_event(sequence));
        if (status == AYNI_BUSY) {
            ++busy;
            const std::vector<Popped> batch = pop_all(queue);
            delivered.insert(delivered.end(), batch.begin(), batch.end());
            continue;
        }
        ASSERT_EQ(status, AYNI_OK);
        ++sequence;
    }
    const std::vector<Popped> rest = pop_all(queue);
    delivered.insert(delivered.end(), rest.begin(), rest.end());

    ASSERT_EQ(delivered.size(), 10000U);
    for (uint64_t index = 0; index < delivered.size(); ++index) {
        ASSERT_EQ(delivered[index].header.sequence, index);
        ASSERT_EQ(delivered[index].body_sequence, index);
    }
    EXPECT_GT(busy, 0U);
    EXPECT_EQ(queue.counters().rejected_busy, busy);
    EXPECT_EQ(queue.counters().replaced, 0U);
    EXPECT_EQ(queue.counters().evicted, 0U);
}

TEST(IngressQueue, full_queue_rejects_other_events_with_busy) {
    QueueStorage storage(4, 16);
    EventQueue &queue = storage.queue;
    for (uint64_t sequence = 0; sequence < 4U; ++sequence) {
        ASSERT_EQ(push(queue, control_event(sequence)), AYNI_OK);
    }
    EXPECT_EQ(push(queue, control_event(4)), AYNI_BUSY);
    EXPECT_EQ(push(queue, telemetry(1, 1, 5)), AYNI_BUSY); // nothing droppable to make room
    EXPECT_EQ(queue.size(), 4U);
    EXPECT_EQ(queue.counters().rejected_busy, 2U);
    const std::vector<Popped> popped = pop_all(queue);
    ASSERT_EQ(popped.size(), 4U);
    EXPECT_EQ(popped.back().header.sequence, 3U);
}

TEST(IngressQueue, full_queue_replaces_newest_value_of_the_same_stream) {
    QueueStorage storage(4, 16);
    EventQueue &queue = storage.queue;
    ASSERT_EQ(push(queue, telemetry(7, 1, 0)), AYNI_OK);
    ASSERT_EQ(push(queue, control_event(1)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(7, 1, 2)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(8, 1, 3)), AYNI_OK); // other subscription, same key
    ASSERT_EQ(push(queue, telemetry(7, 1, 4)), AYNI_OK);

    const std::vector<Popped> popped = pop_all(queue);
    ASSERT_EQ(popped.size(), 4U);
    EXPECT_EQ(popped[0].header.sequence, 0U);
    EXPECT_EQ(popped[1].header.sequence, 1U);
    EXPECT_EQ(popped[2].header.sequence, 4U); // took the place of sequence 2
    EXPECT_EQ(popped[2].body_sequence, 4U);
    EXPECT_EQ(popped[2].header.replaced, 1U);
    EXPECT_EQ(popped[3].header.subscription_id, 8U);
    EXPECT_EQ(queue.counters().replaced, 1U);
}

TEST(IngressQueue, full_queue_evicts_oldest_droppable_event) {
    QueueStorage storage(4, 16);
    EventQueue &queue = storage.queue;
    ASSERT_EQ(push(queue, control_event(0)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(1, 1, 1)), AYNI_OK);
    ASSERT_EQ(push(queue, control_event(2)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(2, 1, 3)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(3, 1, 4)), AYNI_OK); // no matching stream: evict sequence 1

    const std::vector<Popped> popped = pop_all(queue);
    ASSERT_EQ(popped.size(), 4U);
    EXPECT_EQ(popped[0].header.sequence, 0U);
    EXPECT_EQ(popped[1].header.sequence, 2U);
    EXPECT_EQ(popped[2].header.sequence, 3U);
    EXPECT_EQ(popped[3].header.sequence, 4U);
    EXPECT_EQ(queue.counters().evicted, 1U);
}

TEST(IngressQueue, coalesce_key_zero_never_replaces) {
    QueueStorage storage(2, 16);
    EventQueue &queue = storage.queue;
    ASSERT_EQ(push(queue, telemetry(1, 0, 0)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(1, 0, 1)), AYNI_OK);
    ASSERT_EQ(push(queue, telemetry(1, 0, 2)), AYNI_OK);

    const std::vector<Popped> popped = pop_all(queue);
    ASSERT_EQ(popped.size(), 2U);
    EXPECT_EQ(popped[0].header.sequence, 1U);
    EXPECT_EQ(popped[1].header.sequence, 2U);
    EXPECT_EQ(queue.counters().replaced, 0U);
    EXPECT_EQ(queue.counters().evicted, 1U);
}

TEST(IngressQueue, telemetry_flood_keeps_each_stream_in_order_and_accounted) {
    QueueStorage storage(8, 16);
    EventQueue &queue = storage.queue;
    constexpr uint64_t pushed = 10000;
    for (uint64_t sequence = 0; sequence < pushed; ++sequence) {
        ASSERT_EQ(push(queue, telemetry(sequence % 4U, 1, sequence)), AYNI_OK);
    }
    const std::vector<Popped> popped = pop_all(queue);
    const EventQueue::Counters counters = queue.counters();
    EXPECT_EQ(popped.size() + counters.replaced + counters.evicted, pushed);

    std::map<uint64_t, uint64_t> last_by_stream;
    for (const Popped &event : popped) {
        EXPECT_EQ(event.body_sequence, event.header.sequence);
        const auto previous = last_by_stream.find(event.header.subscription_id);
        if (previous != last_by_stream.end()) {
            EXPECT_GT(event.header.sequence, previous->second);
        }
        last_by_stream[event.header.subscription_id] = event.header.sequence;
    }
    ASSERT_EQ(last_by_stream.size(), 4U);
    for (const auto &[stream, last] : last_by_stream) {
        EXPECT_EQ(last, pushed - 4U + stream); // the newest value of each stream survives
    }
}

TEST(IngressQueue, oversized_body_is_invalid_argument) {
    QueueStorage storage(2, 8);
    EventHeader header = control_event(0);
    header.body_size = 9;
    std::array<uint8_t, 9> body{};
    EXPECT_EQ(storage.queue.push(header, body.data()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(storage.queue.size(), 0U);
}

TEST(IngressQueue, empty_body_needs_no_buffer) {
    QueueStorage storage(2, 8);
    EventHeader header = control_event(0);
    header.body_size = 0;
    ASSERT_EQ(storage.queue.push(header, nullptr), AYNI_OK);
    EventHeader out{};
    std::array<uint8_t, 8> body{};
    ASSERT_TRUE(storage.queue.pop(out, body.data()));
    EXPECT_EQ(out.body_size, 0U);
    EXPECT_FALSE(storage.queue.pop(out, body.data()));
}

// Four producer threads post under the Host lock while the owner drains, as
// ayni_post_event and ayni_step will. Producers retry on AYNI_BUSY. A failure
// stops every thread, so the test fails instead of hanging or aborting.
TEST(IngressQueue, concurrent_producers_lose_no_event) {
    ayni::hosts::sim::Host sim;
    const ayni_host_v1 host = sim.table();
    QueueStorage storage(16, 16);
    EventQueue &queue = storage.queue;
    constexpr uint64_t producers = 4;
    constexpr uint64_t per_producer = 2500;

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (uint64_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&, producer] {
            for (uint64_t index = 0; index < per_producer && !failed.load();) {
                EventHeader header = control_event(producer * per_producer + index);
                header.subscription_id = producer;
                ayni_status_t status;
                {
                    ayni::kernel::IngressGuard guard(host);
                    status = push(queue, header);
                }
                if (status == AYNI_OK) {
                    ++index;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::vector<uint64_t> next(producers, 0);
    uint64_t delivered = 0;
    while (delivered < producers * per_producer && !failed.load()) {
        if (std::chrono::steady_clock::now() > deadline) {
            ADD_FAILURE() << "only " << delivered << " events delivered";
            failed.store(true);
            break;
        }
        EventHeader header{};
        std::array<uint8_t, 16> body{};
        bool popped;
        {
            ayni::kernel::IngressGuard guard(host);
            popped = queue.pop(header, body.data());
        }
        if (!popped) {
            std::this_thread::yield();
            continue;
        }
        const uint64_t producer = header.subscription_id;
        if (producer >= producers) {
            ADD_FAILURE() << "event from unknown producer " << producer;
            failed.store(true);
            break;
        }
        EXPECT_EQ(header.sequence, producer * per_producer + next[producer]); // per-producer order
        ++next[producer];
        ++delivered;
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(delivered, producers * per_producer);
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_EQ(sim.calls_under_lock(), 0U);
}

} // namespace
