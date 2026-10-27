#include "kernel/ports/ingress_lock.h"
#include "kernel/runtime/ingress/observation_ingress.h"
namespace ayni::kernel {
namespace {
bool same_opening(ayni_subscription_ref_v1 a, ayni_subscription_ref_v1 b) {
    return a.subscription_id == b.subscription_id && a.boot_epoch == b.boot_epoch &&
           a.generation == b.generation;
}
} // namespace
SubscriptionIngressPort ObservationIngress::subscription_port() {
    return {this,
            [](void *context, uint32_t index, ayni_subscription_ref_v1 reference, uint32_t source,
               uint64_t owner) {
                return static_cast<ObservationIngress *>(context)->prepare_subscription(
                    index, reference, source, owner);
            },
            [](void *context, uint32_t index, ayni_subscription_ref_v1 reference, uint64_t until) {
                return static_cast<ObservationIngress *>(context)->activate_subscription(
                    index, reference, until);
            },
            [](void *context, uint32_t index, ayni_subscription_ref_v1 reference) {
                static_cast<ObservationIngress *>(context)->close_subscription(index, reference);
            }};
}
bool ObservationIngress::owns(const ayni_subscription_sink_t *sink) const {
    return sink && sink->pool == this && sink->index < sessions_.size() &&
           &sessions_[sink->index].target == sink;
}
ayni_status_t ObservationIngress::subscription_admission(uint32_t index,
                                                         ayni_subscription_ref_v1 reference) const {
    const auto status = admission(reference.boot_epoch);
    if (status != AYNI_OK) {
        return status;
    }
    if (index >= sessions_.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &binding = sessions_[index].binding;
    if (!binding || !same_opening(reference, binding->reference)) {
        return AYNI_STALE;
    }
    return binding->until ? AYNI_OK : AYNI_NOT_READY;
}
// The target address survives reuse, while every copied post retains its own
// opening. The owner has already drained the previous generation's borrow.
ayni_subscription_sink_t *
ObservationIngress::prepare_subscription(uint32_t index, ayni_subscription_ref_v1 reference,
                                         uint32_t source_index, uint64_t owner) {
    const IngressGuard guard(host_);
    if (admission(reference.boot_epoch) != AYNI_OK || index >= sessions_.size() ||
        source_index >= registry_.size() || reference.subscription_id == 0 ||
        reference.generation == 0 || owner == 0) {
        return nullptr;
    }
    auto &session = sessions_[index];
    session.binding = ObservationSubscriptionBinding{reference, source_index, owner, {}};
    session.gap.reset();
    return &session.target;
}
bool ObservationIngress::activate_subscription(uint32_t index, ayni_subscription_ref_v1 reference,
                                               uint64_t until) {
    const IngressGuard guard(host_);
    if (admission(reference.boot_epoch) != AYNI_OK || index >= sessions_.size() || until == 0 ||
        until == UINT64_MAX) {
        return false;
    }
    auto &binding = sessions_[index].binding;
    if (!binding || !same_opening(reference, binding->reference)) {
        return false;
    }
    binding->until = until;
    return true;
}
void ObservationIngress::close_subscription(uint32_t index, ayni_subscription_ref_v1 reference) {
    const IngressGuard guard(host_);
    if (index >= sessions_.size()) {
        return;
    }
    auto &binding = sessions_[index].binding;
    if (binding && same_opening(reference, binding->reference)) {
        binding->until.reset();
    }
}
// No owner clock or Task/Grant is read here. Mutable permission and lease are
// rechecked on owner consumption; this gate only proves the opening was confirmed.
ayni_status_t ObservationIngress::post_subscription(uint32_t index,
                                                    ayni_subscription_ref_v1 reference,
                                                    const knowledge::ObservationInput &input) {
    {
        const IngressGuard guard(host_);
        if (const auto status = subscription_admission(index, reference); status != AYNI_OK) {
            return status;
        }
        const auto &binding = *sessions_[index].binding;
        const auto &source = registry_.at(binding.source_index);
        if (input.generation != reference.generation ||
            !input.source.equals({source.id.data(), source.id.size()}) ||
            (input.media && (input.media->boot_epoch != reference.boot_epoch ||
                             input.media->owner_instance_id != binding.owner))) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (const auto status = knowledge::check_observation_source(input, source);
            status != AYNI_OK) {
            return status;
        }
        const auto status =
            enqueue({input, ScopedObservation{reference, index}, binding.source_index});
        if (status != AYNI_OK) {
            return status;
        }
    }
    host_.wake(host_.ctx);
    return AYNI_OK;
}
ayni_status_t ObservationIngress::subscription_gap(uint32_t index,
                                                   ayni_subscription_ref_v1 reference,
                                                   uint64_t through) {
    {
        const IngressGuard guard(host_);
        if (const auto status = subscription_admission(index, reference); status != AYNI_OK) {
            return status;
        }
        auto &gap = sessions_[index].gap;
        const knowledge::ObservationBoundary boundary{reference.generation, through};
        if (!gap ||
            knowledge::after_boundary(boundary.generation, boundary.through, gap->boundary)) {
            gap = ObservationGapCell{boundary};
            ++gap_updates_;
        }
    }
    host_.wake(host_.ctx);
    return AYNI_OK;
}
bool ObservationIngress::take_subscription_gap(uint32_t index, ayni_subscription_ref_v1 &reference,
                                               knowledge::ObservationBoundary &boundary) {
    const IngressGuard guard(host_);
    if (index >= sessions_.size()) {
        return false;
    }
    auto &session = sessions_[index];
    if (!session.binding || !session.gap ||
        session.gap->phase != ObservationGapCell::Phase::pending) {
        return false;
    }
    reference = session.binding->reference;
    boundary = session.gap->boundary;
    session.gap->phase = ObservationGapCell::Phase::applied;
    return true;
}
} // namespace ayni::kernel
