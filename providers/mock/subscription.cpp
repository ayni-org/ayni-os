#include <algorithm>
#include <ayni/providers/mock/subscription.h>
#include <ayni/values.hpp>
#include <cstring>
#include <new>
#include <optional>
namespace ayni::providers::mock {
namespace {
constexpr uint32_t request_capacity_max = 64;
constexpr uint32_t frame_bytes = 3;
constexpr uint32_t contract_version = 1;
constexpr uint32_t request_timeout_ms = 30000;
constexpr uint32_t variant_b_ticks = 2;
constexpr uint64_t no_time = UINT64_MAX;
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
ayni_catalog_name_v1 name(const char *value) {
    ayni_catalog_name_v1 out{};
    out.size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(out.data, value, out.size);
    return out;
}
bool same(ayni_attempt_ref_v1 a, ayni_attempt_ref_v1 b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}
bool same(ayni_subscription_ref_v1 a, ayni_subscription_ref_v1 b) {
    return a.subscription_id == b.subscription_id && a.boot_epoch == b.boot_epoch &&
           a.generation == b.generation;
}
uint64_t after(uint64_t now, uint64_t duration) {
    return duration >= no_time - now ? no_time : now + duration;
}
} // namespace
uint32_t subscription_input_bytes() {
    return text_value_bytes(0);
}
struct SubscriptionProvider::Impl {
    enum class Phase { starting, active, stopping };
    struct Frame {
        ayni_media_ref_v1 reference; // Complete byte identity from the private store.
        uint64_t captured_at_us;     // Original capture, retained across FIFO backpressure.
    };
    struct Slot {
        ayni_attempt_ref_v1 attempt{};       // Positive exactly while occupied.
        ayni_subscription_open_v1 opening{}; // Copied current generation and finite deadline.
        ayni_provider_completion_t *completion = nullptr; // Borrowed until terminal publication.
        Phase phase = Phase::starting;
        uint32_t ticks = 1;
        uint64_t sequence = 0;
        uint64_t lease_sequence = 1;
        uint64_t next_frame_us = 0;
        std::optional<ayni_subscription_renew_v1> renewal;
        std::optional<Frame> frame;
        ayni_call_outcome_t terminal = AYNI_CALL_CANCELLED;
        ayni_status_t terminal_status = AYNI_OK;
    };
    SubscriptionConfig config;
    MediaStore media;
    std::unique_ptr<Slot[]> slots;
    ayni_subscription_contract_v1 subscription{};
    ayni_call_resource_v1 resource{};
    ayni_call_capability_v1 capability{};
    ayni_schema_node_v1 input_schema{}, result_schema{};
    ayni_business_contract_v1 business{};
    ayni_provider_v1 provider{};
    uint64_t now() const { return config.now_us(config.clock_context); }
    void descriptors() {
        subscription = {sizeof(subscription), contract_version, text("mock.camera"),
                        config.lease_ms == 0 ? AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT : config.lease_ms,
                        0};
        resource = {text("camera.slot"), 1, 0};
        capability = {sizeof(capability),
                      contract_version,
                      text("mock.observe"),
                      {sizeof(ayni_call_contract_v1), AYNI_CALL_SUBSCRIPTION, AYNI_CALL_TOOL, 0,
                       AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_CONFIRMED, 0,
                       request_timeout_ms, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT,
                       subscription_input_bytes(), subscription_result_bytes, 1, &resource,
                       &subscription},
                      nullptr};
        input_schema.kind = result_schema.kind = AYNI_VALUE_TEXT;
        input_schema.subtree_end = result_schema.subtree_end = 1;
        business = {{&input_schema, 1}, {&result_schema, 1}, 0, 0, nullptr};
        capability.business = &business;
        const bool a = config.variant == Variant::a;
        provider = {sizeof(provider),
                    contract_version,
                    text(a ? "mock.a.camera" : "mock.b.camera"),
                    text("mock.camera"),
                    {},
                    this,
                    1,
                    0,
                    &capability,
                    submit,
                    cancel,
                    nullptr,
                    renew};
    }
    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (!request || !request->subscription || !completion ||
            request->subscription->struct_size != sizeof(ayni_subscription_open_v1) ||
            !request->subscription->sink || self.now() >= request->subscription->valid_until_us) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        const auto boot = request->subscription->subscription.boot_epoch;
        if (self.media.begin_boot(boot) != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        // Old-generation borrows still owe terminal posts after an in-place reboot.
        // A new boot invalidates bytes, never the completion address before return.
        for (uint32_t index = 0; index < self.config.request_capacity; ++index) {
            auto &old = self.slots[index];
            if (old.completion && old.opening.subscription.boot_epoch != boot) {
                self.stop(old, AYNI_CALL_CANCELLED);
            }
        }
        for (uint32_t index = 0; index < self.config.request_capacity; ++index) {
            auto &slot = self.slots[index];
            if (slot.completion) {
                continue;
            }
            slot = {};
            slot.attempt = request->attempt;
            slot.opening = *request->subscription;
            slot.completion = completion;
            slot.ticks = self.config.variant == Variant::a ? 1U : variant_b_ticks;
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    void stop(Slot &slot, ayni_call_outcome_t terminal, ayni_status_t status = AYNI_OK) {
        slot.phase = Phase::stopping;
        slot.terminal = terminal;
        slot.terminal_status = status;
        slot.frame.reset();
        media.release_generation(text("mock.camera"), slot.opening.subscription.boot_epoch,
                                 slot.opening.subscription.generation);
    }
    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        for (uint32_t index = 0; index < self.config.request_capacity; ++index) {
            auto &slot = self.slots[index];
            if (slot.completion && same(slot.attempt, attempt)) {
                self.stop(slot, AYNI_CALL_CANCELLED);
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static ayni_submit_result_t renew(void *context, const ayni_subscription_renew_v1 *request) {
        auto &self = *static_cast<Impl *>(context);
        if (!request || request->struct_size != sizeof(*request) || request->reserved != 0) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t index = 0; index < self.config.request_capacity; ++index) {
            auto &slot = self.slots[index];
            if (!slot.completion || !same(slot.opening.subscription, request->subscription)) {
                continue;
            }
            if (slot.phase != Phase::active || slot.renewal || slot.lease_sequence == UINT64_MAX ||
                request->sequence != slot.lease_sequence + 1 ||
                self.now() >= slot.opening.valid_until_us ||
                request->valid_until_us <= slot.opening.valid_until_us ||
                request->valid_until_us == no_time) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            slot.renewal = *request;
            slot.opening.valid_until_us = request->valid_until_us;
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_status_t result(Slot &slot, ayni_call_outcome_t outcome, uint64_t sequence) {
        ayni_provider_result_v1 out{};
        out.struct_size = sizeof(out);
        out.outcome = outcome;
        out.attempt = slot.attempt;
        out.sequence = sequence;
        out.status = outcome == AYNI_CALL_FAILED ? slot.terminal_status : AYNI_OK;
        out.effect = slot.sequence == 0 ? AYNI_CALL_EFFECT_NONE : AYNI_CALL_EFFECT_PARTIAL;
        return ayni_provider_post(slot.completion, &out);
    }
    ayni_status_t make_frame(Slot &slot, uint64_t at) {
        if (slot.sequence == UINT64_MAX) {
            stop(slot, AYNI_CALL_FAILED, AYNI_CAPACITY_EXCEEDED);
            return AYNI_OK;
        }
        ++slot.sequence;
        const uint8_t bytes[frame_bytes]{static_cast<uint8_t>(config.variant == Variant::a ? 1 : 0),
                                         static_cast<uint8_t>(slot.sequence), 1};
        ayni_media_ref_v1 metadata{};
        metadata.struct_size = sizeof(metadata);
        metadata.content_type = AYNI_MEDIA_MOCK_BYTES;
        metadata.boot_epoch = slot.opening.subscription.boot_epoch;
        metadata.owner_instance_id = slot.opening.owner_instance_id;
        metadata.generation = slot.opening.subscription.generation;
        metadata.byte_size = frame_bytes;
        const auto ttl = after(at, config.sample_ttl_us);
        metadata.valid_until_us =
            ttl < slot.opening.valid_until_us ? ttl : slot.opening.valid_until_us;
        metadata.source_id = slot.opening.source_id;
        metadata.source_version = slot.opening.source_version;
        metadata.purpose = name("inspect");
        ayni_media_ref_v1 reference{};
        const auto status = media.put(metadata, {bytes, frame_bytes}, at, reference);
        slot.next_frame_us = after(at, config.frame_interval_us);
        if (status == AYNI_OK) {
            slot.frame = Frame{reference, at};
            return AYNI_OK;
        }
        if (status != AYNI_CAPACITY_EXCEEDED) {
            return status;
        }
        const auto gap = ayni_subscription_report_gap(slot.opening.sink, slot.opening.subscription,
                                                      slot.sequence);
        return gap == AYNI_OK || gap == AYNI_NOT_READY || gap == AYNI_STALE ? AYNI_OK : gap;
    }
    ayni_status_t publish_frame(Slot &slot, uint64_t at) {
        if (at >= slot.frame->reference.valid_until_us) {
            slot.frame.reset();
            const auto gap = ayni_subscription_report_gap(slot.opening.sink,
                                                          slot.opening.subscription, slot.sequence);
            return gap == AYNI_OK || gap == AYNI_NOT_READY || gap == AYNI_STALE ? AYNI_OK : gap;
        }
        // Pending frames retain their original capture/TTL while FIFO backpressure lasts.
        const auto captured = slot.frame->captured_at_us;
        const ayni_observation_v1 sample{
            sizeof(sample),
            slot.opening.source_version,
            text("mock.camera"),
            slot.opening.subscription.generation,
            slot.sequence,
            captured,
            config.sample_ttl_us,
            AYNI_OBSERVATION_GOOD,
            0,
            config.change_every_frames == 0
                ? 1
                : static_cast<int64_t>((slot.sequence / config.change_every_frames) % 2)};
        const ayni_subscription_observation_v1 post{sizeof(post), 0, slot.opening.subscription,
                                                    sample, &slot.frame->reference};
        const auto status = ayni_subscription_post_observation(slot.opening.sink, &post);
        if (status == AYNI_OK) {
            slot.frame.reset();
        }
        return status == AYNI_OK || status == AYNI_BUSY || status == AYNI_NOT_READY ||
                       status == AYNI_STALE
                   ? AYNI_OK
                   : status;
    }
    ayni_status_t advance(Slot &slot, uint64_t at) {
        if (slot.phase != Phase::stopping && at >= slot.opening.valid_until_us) {
            stop(slot, AYNI_CALL_FAILED, AYNI_EXPIRED);
        }
        if (slot.phase == Phase::stopping) {
            const auto posted = result(slot, slot.terminal, 0);
            if (posted == AYNI_OK) {
                slot = {};
            }
            return posted;
        }
        if (slot.phase == Phase::starting) {
            if (--slot.ticks != 0) {
                return AYNI_OK;
            }
            const auto posted = result(slot, AYNI_CALL_SUBSCRIPTION_STARTED, 1);
            if (posted == AYNI_OK) {
                slot.phase = Phase::active;
                slot.next_frame_us = at;
            } else {
                slot.ticks = 1;
            }
            return posted;
        }
        if (slot.renewal) {
            const auto posted =
                result(slot, AYNI_CALL_SUBSCRIPTION_RENEWED, slot.renewal->sequence);
            if (posted == AYNI_OK) {
                slot.lease_sequence = slot.renewal->sequence;
                slot.renewal.reset();
            }
            return posted;
        }
        if (!slot.frame && at >= slot.next_frame_us) {
            const auto made = make_frame(slot, at);
            if (made != AYNI_OK || !slot.frame) {
                return made;
            }
        }
        return slot.frame ? publish_frame(slot, at) : AYNI_OK;
    }
};
SubscriptionProvider::SubscriptionProvider() = default;
SubscriptionProvider::~SubscriptionProvider() {
    if (pending() != 0) {
        static_cast<void>(impl_.release());
    }
}
ayni_status_t SubscriptionProvider::configure(SubscriptionConfig config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if ((config.variant != Variant::a && config.variant != Variant::b) || !config.now_us ||
        config.request_capacity == 0 || config.request_capacity > request_capacity_max ||
        config.frame_interval_us == 0 || config.frame_interval_us == no_time ||
        config.sample_ttl_us == 0 || config.sample_ttl_us == no_time) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::unique_ptr<Impl> created(new (std::nothrow) Impl{});
    if (!created) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    created->config = config;
    created->slots.reset(new (std::nothrow) Impl::Slot[config.request_capacity]);
    if (!created->slots) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto status = created->media.configure({config.media_objects, frame_bytes});
    if (status != AYNI_OK) {
        return status;
    }
    created->descriptors();
    impl_ = std::move(created);
    return AYNI_OK;
}
ayni_status_t SubscriptionProvider::describe(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_NOT_READY;
    }
    out = impl_->provider;
    return AYNI_OK;
}
ayni_status_t SubscriptionProvider::describe_source(ayni_observation_source_v1 &out) const {
    if (!impl_) {
        return AYNI_NOT_READY;
    }
    out = {sizeof(out),
           contract_version,
           text("mock.camera"),
           AYNI_OBSERVATION_BOOL,
           AYNI_OBSERVATION_MEASURED,
           text("bool"),
           impl_->config.sample_ttl_us};
    return AYNI_OK;
}
ayni_status_t SubscriptionProvider::advance() {
    if (!impl_) {
        return AYNI_NOT_READY;
    }
    const auto now = impl_->now();
    impl_->media.expire(now);
    for (uint32_t index = 0; index < impl_->config.request_capacity; ++index) {
        auto &slot = impl_->slots[index];
        if (!slot.completion) {
            continue;
        }
        const auto status = impl_->advance(slot, now);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
ayni_status_t SubscriptionProvider::close() {
    if (pending() != 0) {
        return AYNI_BUSY;
    }
    impl_.reset();
    return AYNI_OK;
}
void SubscriptionProvider::abandon_kernel() {
    if (!impl_) {
        return;
    }
    for (uint32_t at = 0; at < impl_->config.request_capacity; ++at) {
        auto &slot = impl_->slots[at];
        if (slot.completion) {
            impl_->stop(slot, AYNI_CALL_CANCELLED);
        }
        slot = {};
    }
}
uint64_t SubscriptionProvider::next_due_us() const {
    if (!impl_) {
        return UINT64_MAX;
    }
    auto due = UINT64_MAX;
    for (uint32_t at = 0; at < impl_->config.request_capacity; ++at) {
        const auto &slot = impl_->slots[at];
        if (!slot.completion) {
            continue;
        }
        const auto next = slot.phase != Impl::Phase::active || slot.renewal
                              ? impl_->now()
                              : std::min(slot.next_frame_us, slot.opening.valid_until_us);
        due = std::min(due, next);
    }
    return due;
}
uint32_t SubscriptionProvider::pending() const {
    uint32_t count = 0;
    if (impl_) {
        for (uint32_t index = 0; index < impl_->config.request_capacity; ++index) {
            count += impl_->slots[index].completion ? 1U : 0U;
        }
    }
    return count;
}
const MediaStore *SubscriptionProvider::media_store() const {
    return impl_ ? &impl_->media : nullptr;
}
} // namespace ayni::providers::mock
