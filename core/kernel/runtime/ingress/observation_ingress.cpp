#include "kernel/runtime/ingress/observation_ingress.h"
#include "kernel/ports/ingress_lock.h"
#include <cstring>
#include <type_traits>
namespace ayni::kernel {
namespace {
constexpr uint32_t sample_kind = 1;
// The same placement calculation serves measurement and construction.
std::size_t gaps_offset(uint32_t capacity) {
    const auto bytes = EventQueue::storage_size(capacity, sizeof(QueuedObservation));
    constexpr auto alignment = alignof(std::optional<ObservationGapCell>);
    return (bytes + alignment - 1) / alignment * alignment;
}
} // namespace
std::size_t ObservationIngress::sessions_offset(uint32_t capacity, uint32_t sources) {
    const auto bytes = gaps_offset(capacity) + foundation::BoundedArray<Gap>::storage_size(sources);
    constexpr auto alignment = alignof(ObservationSubscriptionCell);
    return (bytes + alignment - 1) / alignment * alignment;
}
std::size_t ObservationIngress::storage_size(uint32_t capacity, uint32_t sources,
                                             uint32_t subscriptions) {
    return capacity == 0 ? 0
                         : sessions_offset(capacity, sources) +
                               foundation::BoundedArray<ObservationSubscriptionCell>::storage_size(
                                   subscriptions);
}
ObservationIngress::ObservationIngress(void *storage, uint32_t capacity, uint32_t sources,
                                       const catalog::ObservationSources &registry,
                                       const ayni_host_v1 &host, uint32_t subscriptions)
    : registry_(registry), host_(host), capacity_(capacity),
      sessions_(capacity == 0
                    ? nullptr
                    : static_cast<uint8_t *>(storage) + sessions_offset(capacity, sources),
                capacity == 0 ? 0 : subscriptions),
      gaps_(capacity == 0 ? nullptr : static_cast<uint8_t *>(storage) + gaps_offset(capacity),
            capacity == 0 ? 0 : sources) {
    static_assert(std::is_trivially_copyable<QueuedObservation>::value,
                  "internal queue body must be copied");
    if (capacity != 0) {
        queue_.emplace(storage, capacity, sizeof(QueuedObservation));
        while (sessions_.size() < subscriptions) {
            ObservationSubscriptionCell cell{};
            cell.target = {this, sessions_.size()};
            static_cast<void>(sessions_.push_back(cell));
        }
        while (gaps_.size() < sources) {
            static_cast<void>(gaps_.push_back({}));
        }
    }
}
ayni_status_t ObservationIngress::admission(uint64_t boot_epoch) const {
    return capacity_ == 0         ? AYNI_UNSUPPORTED
           : !boot_               ? AYNI_NOT_READY
           : boot_epoch != *boot_ ? AYNI_STALE
                                  : AYNI_OK;
}
void ObservationIngress::begin_boot(uint64_t boot_epoch) {
    const IngressGuard guard(host_);
    if (capacity_ != 0) {
        boot_ = boot_epoch;
    }
}
// Gate closure linearizes before discarding. Each subsequent critical section
// touches just one queue item or gap cell (C-HOST-020), never a frame buffer.
void ObservationIngress::close() {
    {
        const IngressGuard guard(host_);
        boot_.reset();
    }
    for (;;) {
        const IngressGuard guard(host_);
        if (!queue_ || !queue_->discard()) {
            break;
        }
        ++discarded_;
    }
    for (uint32_t index = 0; index < gaps_.size(); ++index) {
        const IngressGuard guard(host_);
        gaps_[index].reset();
    }
    for (uint32_t index = 0; index < sessions_.size(); ++index) {
        const IngressGuard guard(host_);
        sessions_[index].binding.reset();
        sessions_[index].gap.reset();
    }
}
ayni_status_t ObservationIngress::post(uint64_t boot_epoch,
                                       const knowledge::ObservationInput &input) {
    const IngressGuard guard(host_);
    if (const auto gated = admission(boot_epoch); gated != AYNI_OK) {
        return gated;
    }
    if (input.media && input.media->boot_epoch != boot_epoch) {
        return AYNI_STALE;
    }
    uint32_t index = 0;
    if (!registry_.find({input.source.data(), input.source.size()}, index)) {
        return AYNI_NOT_FOUND;
    }
    if (const auto status = knowledge::check_observation_source(input, registry_.at(index));
        status != AYNI_OK) {
        return status;
    }
    return enqueue({input, {}, index});
}
ayni_status_t ObservationIngress::enqueue(const QueuedObservation &queued) {
    EventHeader header{};
    header.kind = sample_kind;
    header.body_size = sizeof(queued);
    // This class remains non-droppable; no sample may overwrite a control receipt.
    const auto status = queue_->push(header, reinterpret_cast<const uint8_t *>(&queued));
    if (status == AYNI_OK) {
        ++accepted_;
    }
    return status;
}
ayni_status_t ObservationIngress::gap(uint64_t boot_epoch, const catalog::CatalogId &source,
                                      uint32_t version, knowledge::ObservationBoundary boundary) {
    const IngressGuard guard(host_);
    if (const auto gated = admission(boot_epoch); gated != AYNI_OK) {
        return gated;
    }
    uint32_t index = 0;
    if (!registry_.find({source.data(), source.size()}, index)) {
        return AYNI_NOT_FOUND;
    }
    if (registry_.at(index).version != version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    auto &old = gaps_[index];
    if (!old || knowledge::after_boundary(boundary.generation, boundary.through, old->boundary)) {
        old = ObservationGapCell{boundary};
        ++gap_updates_;
    }
    return AYNI_OK;
}
bool ObservationIngress::take_gap(uint32_t source_index, knowledge::ObservationBoundary &boundary) {
    const IngressGuard guard(host_);
    if (source_index >= gaps_.size()) {
        return false;
    }
    auto &gap = gaps_[source_index];
    if (!gap || gap->phase != ObservationGapCell::Phase::pending) {
        return false;
    }
    boundary = gap->boundary;
    gap->phase = ObservationGapCell::Phase::applied;
    return true;
}
uint32_t ObservationIngress::queued() const {
    if (capacity_ == 0) {
        return 0;
    }
    const IngressGuard guard(host_);
    return queue_->size();
}
bool ObservationIngress::take(QueuedObservation &out) {
    const IngressGuard guard(host_);
    EventHeader header{};
    return queue_ && queue_->pop(header, reinterpret_cast<uint8_t *>(&out));
}
void ObservationIngress::discarded() {
    const IngressGuard guard(host_);
    ++discarded_;
}
ayni_observation_ingress_info_v1 ObservationIngress::info() const {
    const IngressGuard guard(host_);
    uint32_t pending = 0;
    for (uint32_t index = 0; index < gaps_.size(); ++index) {
        pending +=
            gaps_[index] && gaps_[index]->phase == ObservationGapCell::Phase::pending ? 1U : 0U;
    }
    for (uint32_t index = 0; index < sessions_.size(); ++index) {
        const auto &gap = sessions_[index].gap;
        pending += gap && gap->phase == ObservationGapCell::Phase::pending ? 1U : 0U;
    }
    return {sizeof(ayni_observation_ingress_info_v1),
            capacity_,
            queue_ ? queue_->size() : 0,
            pending,
            accepted_,
            queue_ ? queue_->counters().rejected_busy : 0,
            gap_updates_,
            discarded_};
}
} // namespace ayni::kernel
