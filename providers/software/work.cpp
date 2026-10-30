#include <array>
#include <ayni/providers/software/work.h>
#include <ayni/values.hpp>
#include <cstring>
#include <new>
#include <optional>
#include <string_view>
namespace ayni::providers::software {
namespace {
constexpr uint32_t capability_count = 2;
enum Offer : uint32_t { sum_offer, events_offer };
constexpr uint32_t version = 1;
constexpr uint32_t timeout_ms = 60000;
constexpr uint32_t value_bound = 64;
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
bool same(ayni_attempt_ref_v1 a, ayni_attempt_ref_v1 b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}
bool same(ayni_subscription_ref_v1 a, ayni_subscription_ref_v1 b) {
    return a.subscription_id == b.subscription_id && a.boot_epoch == b.boot_epoch &&
           a.generation == b.generation;
}
uint64_t deadline(uint64_t now, uint64_t duration) {
    return duration >= UINT64_MAX - now ? UINT64_MAX : now + duration;
}
} // namespace
struct WorkProvider::Impl {
    // All request views are copied. A terminal request retains its borrow until post succeeds.
    struct Request {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        enum class Kind { sum, stream } kind = Kind::sum;
        enum class Phase { starting, running, terminal } phase = Phase::starting;
        uint32_t items = 0;     // Positive only for sum; zero explicitly unused for stream.
        uint32_t processed = 0; // 0..items, sum of exactly this completed prefix.
        int64_t sum = 0;
        ayni_subscription_open_v1 opening{}; // All zero for sum; copied finite opening for stream.
        uint64_t sequence = 0;               // Progress/sample sequence; zero before first post.
        uint64_t next_sample_us = 0;         // Stream's next eligibility; zero before STARTED.
        uint64_t lease_sequence = 1;         // STARTED is one; renewals strictly increment.
        std::optional<ayni_subscription_renew_v1> renewal; // Reliable ACK still owed.
        std::optional<ayni_observation_v1>
            sample; // Original capture retained under FIFO backpressure.
        ayni_call_outcome_t outcome = AYNI_CALL_CANCELLED; // Used only in terminal phase.
        ayni_status_t status = AYNI_OK; // Used only in terminal phase; never a raw backend error.
    };
    WorkConfig config;
    std::array<std::optional<Request>, capacity_max> requests;
    std::array<ayni_schema_node_v1, capability_count> inputs{}, results{};
    std::array<ayni_business_contract_v1, capability_count> business{};
    std::array<ayni_call_capability_v1, capability_count> offers{};
    ayni_call_resource_v1 resource{text("software.slot"), 1, 0};
    ayni_subscription_contract_v1 subscription{sizeof(subscription), version,
                                               text("software.counter"),
                                               AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT, 0};
    uint64_t now() const { return config.now_us(config.clock_context); }
    void declarations() {
        inputs[sum_offer].kind = results[sum_offer].kind = AYNI_VALUE_I64;
        inputs[sum_offer].subtree_end = results[sum_offer].subtree_end = 1;
        inputs[sum_offer].unit = text("items");
        inputs[sum_offer].min_i64 = 1;
        inputs[sum_offer].max_i64 = item_count_max;
        results[sum_offer].unit = text("sum");
        results[sum_offer].max_i64 = item_count_max * (item_count_max + 1) / 2;
        inputs[events_offer].kind = results[events_offer].kind = AYNI_VALUE_TEXT;
        inputs[events_offer].subtree_end = results[events_offer].subtree_end = 1;
        for (uint32_t at = 0; at < capability_count; ++at) {
            business[at] = {{&inputs[at], 1}, {&results[at], 1}, 0, 0, nullptr};
            auto &offer = offers[at];
            offer.struct_size = sizeof(offer);
            offer.version = version;
            offer.capability_id = text(at == sum_offer ? "software.sum" : "software.events");
            offer.contract = {sizeof(ayni_call_contract_v1),
                              at == sum_offer ? AYNI_CALL_ACTION : AYNI_CALL_SUBSCRIPTION,
                              AYNI_CALL_TOOL,
                              0,
                              AYNI_CALL_IDEMPOTENT,
                              AYNI_CALL_QUERY_NONE,
                              AYNI_CALL_CANCEL_CONFIRMED,
                              0,
                              timeout_ms,
                              AYNI_CALL_INPUT_VALUE,
                              at == sum_offer ? AYNI_CALL_RESULT_VALUE : AYNI_CALL_RESULT_TEXT,
                              value_bound,
                              value_bound,
                              1,
                              &resource,
                              at == events_offer ? &subscription : nullptr};
            offer.business = &business[at];
        }
    }
    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *input,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (!input || !completion || input->struct_size != sizeof(*input) ||
            input->execution_permit) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        Request request{};
        request.attempt = input->attempt;
        request.completion = completion;
        if (std::string_view(input->capability_id.data, input->capability_id.size) ==
            "software.sum") {
            if (input->subscription || input->value.node_count != 1 ||
                input->value.nodes[0].kind != AYNI_VALUE_I64 ||
                input->value.nodes[0].value.integer < 1 ||
                input->value.nodes[0].value.integer > item_count_max) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            request.items = static_cast<uint32_t>(input->value.nodes[0].value.integer);
            request.phase = Request::Phase::running;
        } else if (std::string_view(input->capability_id.data, input->capability_id.size) ==
                   "software.events") {
            if (!input->subscription || !input->subscription->sink ||
                self.now() >= input->subscription->valid_until_us) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            request.kind = Request::Kind::stream;
            request.opening = *input->subscription;
        } else {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t at = 0; at < self.config.capacity; ++at) {
            if (!self.requests[at]) {
                self.requests[at] = request;
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        for (auto &slot : self.requests) {
            if (slot && same(slot->attempt, attempt)) {
                if (slot->phase == Request::Phase::terminal) {
                    return AYNI_SUBMIT_ACCEPTED; // Preserve a computed result still awaiting
                                                 // terminal publication.
                }
                slot->phase = Request::Phase::terminal;
                slot->outcome = AYNI_CALL_CANCELLED;
                slot->status = AYNI_OK;
                slot->sample.reset();
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    static ayni_submit_result_t renew(void *context, const ayni_subscription_renew_v1 *renewal) {
        auto &self = *static_cast<Impl *>(context);
        if (!renewal || renewal->struct_size != sizeof(*renewal) || renewal->reserved) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (auto &slot : self.requests) {
            if (!slot || slot->kind != Request::Kind::stream ||
                !same(slot->opening.subscription, renewal->subscription)) {
                continue;
            }
            if (slot->phase != Request::Phase::running || slot->renewal ||
                slot->lease_sequence == UINT64_MAX ||
                renewal->sequence != slot->lease_sequence + 1 ||
                self.now() >= slot->opening.valid_until_us ||
                renewal->valid_until_us <= slot->opening.valid_until_us ||
                renewal->valid_until_us == UINT64_MAX) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            slot->renewal = *renewal;
            slot->opening.valid_until_us = renewal->valid_until_us;
            return AYNI_SUBMIT_ACCEPTED;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_status_t post(Request &request, ayni_call_outcome_t outcome, uint64_t sequence = 0) {
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = outcome;
        result.attempt = request.attempt;
        result.sequence = sequence;
        result.status = request.status;
        result.effect = outcome == AYNI_CALL_COMPLETED
                            ? AYNI_CALL_EFFECT_COMPLETE
                            : AYNI_CALL_EFFECT_NONE; // Completion proves all declared computation,
                                                     // cancellation proves no external effect.
        ayni::ValueBuilder<1> value;
        if (outcome == AYNI_CALL_COMPLETED) {
            const auto status = value.integer(0, request.sum);
            if (status != AYNI_OK || value.finish(result.value) != AYNI_OK) {
                return AYNI_INVALID_STATE;
            }
        }
        return ayni_provider_post(request.completion, &result);
    }
    ayni_status_t advance_stream(Request &request) {
        const auto time = now();
        if (time >= request.opening.valid_until_us) {
            request.phase = Request::Phase::terminal;
            return AYNI_OK;
        }
        if (request.phase == Request::Phase::starting || request.renewal) {
            const bool start = request.phase == Request::Phase::starting;
            const auto sequence = start ? 1U : request.renewal->sequence;
            const auto status = post(
                request, start ? AYNI_CALL_SUBSCRIPTION_STARTED : AYNI_CALL_SUBSCRIPTION_RENEWED,
                sequence);
            if (status == AYNI_OK) {
                request.phase = Request::Phase::running;
                request.lease_sequence = sequence;
                request.renewal.reset();
                if (start) {
                    request.next_sample_us = deadline(time, config.event_interval_us);
                }
            }
            return status;
        }
        if (!request.sample && time >= request.next_sample_us) {
            if (request.sequence >= static_cast<uint64_t>(INT64_MAX)) {
                request.phase = Request::Phase::terminal;
                request.outcome = AYNI_CALL_FAILED;
                request.status = AYNI_CAPACITY_EXCEEDED;
                return AYNI_OK;
            }
            ++request.sequence;
            request.sample = ayni_observation_v1{sizeof(ayni_observation_v1),
                                                 version,
                                                 text("software.counter"),
                                                 request.opening.subscription.generation,
                                                 request.sequence,
                                                 time,
                                                 config.sample_ttl_us,
                                                 AYNI_OBSERVATION_GOOD,
                                                 0,
                                                 static_cast<int64_t>(request.sequence)};
            request.next_sample_us = deadline(time, config.event_interval_us);
        }
        if (!request.sample) {
            return AYNI_OK;
        }
        if (time - request.sample->captured_at_us >= config.sample_ttl_us) {
            const auto gap = ayni_subscription_report_gap(
                request.opening.sink, request.opening.subscription, request.sample->sequence);
            if (gap == AYNI_OK || gap == AYNI_NOT_READY || gap == AYNI_STALE) {
                request.sample.reset();
                return AYNI_OK;
            }
            return gap;
        }
        const ayni_subscription_observation_v1 sample{
            sizeof(sample), 0, request.opening.subscription, *request.sample, nullptr};
        const auto status = ayni_subscription_post_observation(request.opening.sink, &sample);
        if (status == AYNI_OK) {
            request.sample.reset();
        }
        return status == AYNI_BUSY || status == AYNI_NOT_READY || status == AYNI_STALE ? AYNI_OK
                                                                                       : status;
    }
    ayni_status_t advance(std::optional<Request> &slot) {
        auto &request = *slot;
        if (request.phase == Request::Phase::terminal) {
            const auto status = post(request, request.outcome);
            if (status == AYNI_OK) {
                slot.reset();
            }
            return status;
        }
        if (request.kind == Request::Kind::stream) {
            return advance_stream(request);
        }
        for (uint32_t at = 0; at < config.items_per_advance && request.processed < request.items;
             ++at) {
            request.sum += ++request.processed;
        }
        if (request.processed == request.items) {
            request.phase = Request::Phase::terminal;
            request.outcome = AYNI_CALL_COMPLETED;
            return AYNI_OK;
        }
        return post(request, AYNI_CALL_PROGRESS, request.processed);
    }
};
WorkProvider::WorkProvider() = default;
WorkProvider::~WorkProvider() {
    if (pending()) {
        static_cast<void>(
            impl_.release()); // A failed drain cannot invalidate outstanding Kernel borrows.
    }
}
ayni_status_t WorkProvider::configure(WorkConfig config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if (!config.now_us || !config.capacity || config.capacity > capacity_max ||
        !config.items_per_advance || config.items_per_advance > chunk_max ||
        !config.event_interval_us || !config.sample_ttl_us ||
        config.event_interval_us == UINT64_MAX || config.sample_ttl_us == UINT64_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto *value = new (std::nothrow) Impl;
    if (!value) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    value->config = config;
    value->declarations();
    impl_.reset(value);
    return AYNI_OK;
}
ayni_status_t WorkProvider::describe(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = {sizeof(out),
           version,
           text("software.work"),
           text("software"),
           {},
           impl_.get(),
           capability_count,
           0,
           impl_->offers.data(),
           Impl::submit,
           Impl::cancel,
           nullptr,
           Impl::renew};
    return AYNI_OK;
}
ayni_status_t WorkProvider::describe_source(ayni_observation_source_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = {sizeof(out),
           version,
           text("software.counter"),
           AYNI_OBSERVATION_I64,
           AYNI_OBSERVATION_MEASURED,
           text("count"),
           impl_->config.sample_ttl_us};
    return AYNI_OK;
}

ayni_status_t WorkProvider::usage(ayni_string_v1 capability, const ayni_tool_ref_v1 &binding,
                                  ayni_tool_usage_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    if (!capability.data || !binding.binding_id.data || !binding.binding_id.size ||
        !binding.binding_version || binding.role != AYNI_TOOL_ROLE_CAPABILITY) {
        return AYNI_INVALID_ARGUMENT;
    }
    const std::string_view id(capability.data, capability.size);
    if (id != "software.sum" && id != "software.events") {
        return AYNI_NOT_FOUND;
    }
    const bool sum = id == "software.sum";
    out = {
        sizeof(out),
        binding.binding_version,
        binding.binding_id,
        version,
        capability,
        text(sum ? "Compute an exact bounded sum without devices or network."
                 : "Observe a finite-lease software counter stream."),
        text(sum ? "Input is the positive integer item count from the Schema."
                 : "Input is empty text; read samples through the Task-owned operation."),
        text(sum ? "Exact I64 sum of integers from one through the input; progress is not a result."
                 : "Fresh measured I64 counter samples; close returns a cancellation terminal."),
        text(sum ? "Input 512 returns 131328 after completion."
                 : "Start with empty text, wait for a usable sample, then close."),
        0,
        0,
        nullptr};
    return AYNI_OK;
}
ayni_status_t WorkProvider::advance() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    for (auto &slot : impl_->requests) {
        if (slot) {
            const auto status = impl_->advance(slot);
            if (status != AYNI_OK && status != AYNI_BUSY) {
                return status;
            }
        }
    }
    return AYNI_OK;
}
uint32_t WorkProvider::pending() const {
    uint32_t count = 0;
    if (impl_) {
        for (const auto &slot : impl_->requests) {
            count += slot ? 1U : 0U;
        }
    }
    return count;
}
ayni_status_t WorkProvider::close() {
    if (pending()) {
        return AYNI_BUSY;
    }
    impl_.reset();
    return AYNI_OK;
}
} // namespace ayni::providers::software
