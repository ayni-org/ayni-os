#include "kernel/runtime/ingress/event_queue.h"

#include <cstring>

namespace ayni::kernel {
namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

// Header followed by the largest body, padded so every header stays aligned.
std::size_t entry_stride(uint32_t body_max_bytes) {
    return align_up(sizeof(EventHeader) + body_max_bytes, alignof(EventHeader));
}

bool same_stream(const EventHeader &a, const EventHeader &b) {
    return a.subscription_id == b.subscription_id && a.coalesce_key == b.coalesce_key;
}

} // namespace

std::size_t EventQueue::storage_size(uint32_t capacity, uint32_t body_max_bytes) {
    // Entries, then the order ring and the free stack, one index each per entry.
    return entry_stride(body_max_bytes) * capacity + 2U * sizeof(uint16_t) * capacity;
}

EventQueue::EventQueue(void *storage, uint32_t capacity, uint32_t body_max_bytes)
    : entries_(static_cast<unsigned char *>(storage)), stride_(entry_stride(body_max_bytes)),
      order_(reinterpret_cast<uint16_t *>(entries_ + stride_ * capacity)), free_(order_ + capacity),
      capacity_(capacity), body_max_bytes_(body_max_bytes), free_count_(capacity) {
    for (uint32_t index = 0; index < capacity; ++index) {
        free_[index] = static_cast<uint16_t>(capacity - 1U - index);
    }
}

ayni_status_t EventQueue::push(const EventHeader &header, const uint8_t *body) {
    if (header.body_size > body_max_bytes_) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (count_ == capacity_) {
        const Room room = header.droppable ? find_room(header) : Room{Room::Kind::none, 0, 0};
        if (room.kind == Room::Kind::none) {
            ++counters_.rejected_busy;
            return AYNI_BUSY;
        }
        if (room.kind == Room::Kind::replace) {
            write(at_position(room.position), header, room.replaced + 1U, body);
            ++counters_.replaced;
            return AYNI_OK;
        }
        remove_at(room.position);
        ++counters_.evicted;
    }
    const uint16_t index = free_[--free_count_];
    write(index, header, 0, body);
    at_position(count_) = index;
    ++count_;
    return AYNI_OK;
}

bool EventQueue::pop(EventHeader &out_header, uint8_t *body) {
    if (count_ == 0) {
        return false;
    }
    const uint16_t index = at_position(0);
    out_header = header_of(index);
    if (out_header.body_size > 0U) {
        std::memcpy(body, entry(index) + sizeof(EventHeader), out_header.body_size);
    }
    static_cast<void>(take_head());
    return true;
}
bool EventQueue::discard() {
    if (count_ == 0) {
        return false;
    }
    static_cast<void>(take_head());
    return true;
}
uint16_t EventQueue::take_head() {
    const auto index = at_position(0);
    head_ = (head_ + 1U) % capacity_;
    --count_;
    free_[free_count_++] = index;
    return index;
}

void EventQueue::write(uint16_t index, const EventHeader &header, uint32_t replaced,
                       const uint8_t *body) {
    EventHeader stored = header;
    stored.replaced = replaced;
    unsigned char *slot = entry(index);
    std::memcpy(slot, &stored, sizeof(stored));
    if (header.body_size > 0U) {
        std::memcpy(slot + sizeof(EventHeader), body, header.body_size);
    }
}

EventHeader EventQueue::header_of(uint16_t index) const {
    EventHeader header{};
    std::memcpy(&header, entry(index), sizeof(header));
    return header;
}

// One pass from newest to oldest. The newest value of the same stream is
// overwritten in place, so older values of the stream stay ahead of it and a
// stream is still delivered in posting order. Without one, the oldest
// droppable entry is evicted.
EventQueue::Room EventQueue::find_room(const EventHeader &header) const {
    Room room{Room::Kind::none, 0, 0};
    for (uint32_t position = count_; position > 0U; --position) {
        const EventHeader queued = header_of(at_position(position - 1U));
        if (!queued.droppable) {
            continue;
        }
        if (header.coalesce_key != 0U && same_stream(queued, header)) {
            return Room{Room::Kind::replace, position - 1U, queued.replaced};
        }
        room = Room{Room::Kind::evict, position - 1U, 0};
    }
    return room;
}

void EventQueue::remove_at(uint32_t position) {
    const uint16_t index = at_position(position);
    for (uint32_t later = position; later + 1U < count_; ++later) {
        at_position(later) = at_position(later + 1U);
    }
    --count_;
    free_[free_count_++] = index;
}

} // namespace ayni::kernel
