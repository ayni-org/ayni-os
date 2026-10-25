#include "kernel/ingress/event_ingress.h"

#include "kernel/ingress/ingress_lock.h"

#include <array>

namespace ayni::kernel {
namespace {

// The body bytes of each entry: room for a body, and at least for a source.
uint32_t entry_body_bytes(uint32_t body_max_bytes) {
    return body_max_bytes > catalog::CatalogId::max_size ? body_max_bytes
                                                         : catalog::CatalogId::max_size;
}

} // namespace

std::size_t EventIngress::storage_size(uint32_t capacity, uint32_t body_max_bytes) {
    return EventQueue::storage_size(capacity, entry_body_bytes(body_max_bytes));
}

EventIngress::EventIngress(void *storage, uint32_t capacity, uint32_t body_max_bytes,
                           const ayni_host_v1 &host)
    : host_(host), queue_(storage, capacity, entry_body_bytes(body_max_bytes)) {}

ayni_status_t EventIngress::post_local_input(const catalog::CatalogId &source) {
    // Never droppable: a full queue refuses it (C-SUB-004).
    EventHeader header{};
    header.kind = AYNI_EVENT_LOCAL_INPUT;
    header.body_size = source.size();
    const IngressGuard guard(host_);
    if (!open_) {
        return AYNI_INVALID_STATE;
    }
    if (accepted_.through == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    header.sequence = accepted_.through + 1U;
    const auto status = queue_.push(header, reinterpret_cast<const uint8_t *>(source.data()));
    if (status == AYNI_OK) {
        accepted_.through = header.sequence;
    }
    return status;
}

void EventIngress::establish_incarnation(uint64_t boot_epoch) {
    const IngressGuard guard(host_);
    if (accepted_.incarnation == 0U) {
        accepted_.incarnation = boot_epoch;
    }
}

work::InputResumeBoundary EventIngress::resume_boundary() const {
    const IngressGuard guard(host_);
    return accepted_;
}

void EventIngress::open() {
    const IngressGuard guard(host_);
    open_ = true;
}

uint32_t EventIngress::close() {
    const IngressGuard guard(host_);
    open_ = false;
    return queue_.size();
}

uint32_t EventIngress::queued() const {
    const IngressGuard guard(host_);
    return queue_.size();
}

bool EventIngress::take_local_input(catalog::CatalogId &out_source,
                                    work::InputResumeBoundary *out_position) {
    EventHeader header{};
    // post_local_input() is the only push, so every entry holds a source,
    // which fits here.
    std::array<uint8_t, catalog::CatalogId::max_size> body{};
    {
        const IngressGuard guard(host_);
        if (!queue_.pop(header, body.data())) {
            return false;
        }
        if (out_position != nullptr) {
            *out_position = {accepted_.incarnation, header.sequence};
        }
    }
    // post_local_input() queued the bytes of a catalog id.
    static_cast<void>(
        out_source.assign({reinterpret_cast<const char *>(body.data()), header.body_size}));
    return true;
}

} // namespace ayni::kernel
