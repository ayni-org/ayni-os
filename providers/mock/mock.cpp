#include <ayni/providers/mock.h>

#include <cstring>
#include <new>
#include <utility>

namespace ayni::providers::mock {
namespace {
constexpr uint32_t request_capacity_max = 64;
constexpr uint32_t text_bytes_max = 4096;
constexpr uint32_t contract_version = 1;
constexpr uint32_t request_timeout_ms = 30000;
constexpr uint32_t variant_b_ticks = 2;
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
bool same(ayni_string_v1 left, ayni_string_v1 right) {
    return left.size == right.size && left.data != nullptr &&
           std::memcmp(left.data, right.data, left.size) == 0;
}
bool same(ayni_attempt_ref_v1 left, ayni_attempt_ref_v1 right) {
    return left.boot_epoch == right.boot_epoch && left.request_id == right.request_id;
}
} // namespace

struct Provider::Impl {
    // Only terminal phases may retry publication; they never execute the operation again.
    enum class Phase { empty, waiting, progressed, succeeded, cancelled };
    struct Slot {
        Phase phase = Phase::empty;
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
        uint32_t ticks = 0;
        uint32_t text_size = 0;
    };
    explicit Impl(Config config_value) : config(config_value) {}

    Config config;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<char[]> bodies;
    ayni_call_resource_v1 resource{};
    ayni_call_capability_v1 capability{};
    ayni_provider_v1 descriptor{};
    uint32_t active = 0;
    uint64_t executed = 0;

    char *body(uint32_t index) { return bodies.get() + index * config.text_max_bytes; }

    void build_descriptor() {
        const bool model = config.service == Service::text;
        const bool a = config.variant == Variant::a;
        resource = {text(model ? "model.slot" : "presentation.slot"), 1, 0};
        capability = {};
        capability.struct_size = sizeof(capability);
        capability.version = contract_version;
        capability.capability_id = text(model ? "mock.text" : "mock.present");
        capability.contract = {sizeof(capability.contract),
                               model ? AYNI_CALL_UNARY : AYNI_CALL_ACTION,
                               model ? AYNI_CALL_MODEL : AYNI_CALL_TOOL,
                               0,
                               model ? AYNI_CALL_IDEMPOTENT : AYNI_CALL_NOT_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_REQUEST,
                               0,
                               request_timeout_ms,
                               AYNI_CALL_INPUT_FIELDS_TEXT,
                               AYNI_CALL_RESULT_TEXT,
                               config.text_max_bytes,
                               config.text_max_bytes,
                               1,
                               &resource};
        descriptor = {sizeof(descriptor),
                      contract_version,
                      text(model ? (a ? "mock.a.text" : "mock.b.text")
                                 : (a ? "mock.a.present" : "mock.b.present")),
                      text(model ? (a ? "mock.a.model" : "mock.b.model")
                                 : (a ? "mock.a.output" : "mock.b.output")),
                      {},
                      this,
                      1,
                      0,
                      &capability,
                      submit,
                      cancel,
                      nullptr};
    }

    static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<Impl *>(context);
        if (request == nullptr || completion == nullptr ||
            request->struct_size != sizeof(*request) ||
            request->capability_version != contract_version || request->attempt.boot_epoch == 0 ||
            request->attempt.request_id == 0 || request->logical_call_id == 0 ||
            request->operation_id == 0 ||
            !same(request->capability_id, self.capability.capability_id) ||
            !same(request->endpoint_id, self.descriptor.endpoint_id) ||
            request->arguments.field_count != 0 ||
            request->text.size > self.config.text_max_bytes ||
            (request->text.size != 0 && request->text.data == nullptr)) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t at = 0; at < self.config.request_capacity; ++at) {
            auto &slot = self.slots[at];
            if (slot.phase == Phase::empty) {
                if (request->text.size != 0) {
                    std::memcpy(self.body(at), request->text.data, request->text.size);
                }
                slot = {Phase::waiting, request->attempt, completion,
                        self.config.variant == Variant::a ? 1U : variant_b_ticks,
                        request->text.size};
                ++self.active;
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }

    static ayni_submit_result_t cancel(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<Impl *>(context);
        for (uint32_t at = 0; at < self.config.request_capacity; ++at) {
            auto &slot = self.slots[at];
            if (slot.phase != Phase::empty && same(slot.attempt, attempt)) {
                if (slot.phase == Phase::waiting || slot.phase == Phase::progressed) {
                    slot.phase = Phase::cancelled;
                }
                // An already executed result remains success; cancellation acceptance
                // is not proof that the business action stopped without effects.
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }

    ayni_status_t advance(uint32_t index) {
        auto &slot = slots[index];
        if (slot.phase == Phase::empty) {
            return AYNI_OK;
        }
        if (slot.phase == Phase::waiting) {
            if (--slot.ticks != 0) {
                return AYNI_OK;
            }
            if (config.service == Service::action) {
                const ayni_provider_result_v1 progress{sizeof(progress),
                                                       AYNI_CALL_PROGRESS,
                                                       slot.attempt,
                                                       1,
                                                       AYNI_OK,
                                                       AYNI_REASON_NONE,
                                                       AYNI_CALL_EFFECT_NONE,
                                                       0,
                                                       {}};
                const auto status = ayni_provider_post(slot.completion, &progress);
                if (status == AYNI_OK) {
                    slot.phase = Phase::progressed;
                } else {
                    slot.ticks = 1; // Publication refused the progress; a later tick may retry it.
                }
                return status;
            }
        }
        if (slot.phase == Phase::waiting || slot.phase == Phase::progressed) {
            ++executed;
            slot.phase = Phase::succeeded;
        }
        const bool success = slot.phase == Phase::succeeded;
        const ayni_provider_result_v1 result{
            sizeof(result),
            success ? AYNI_CALL_COMPLETED : AYNI_CALL_CANCELLED,
            slot.attempt,
            0,
            AYNI_OK,
            AYNI_REASON_NONE,
            success ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_NONE,
            0,
            success ? ayni_string_v1{body(index), slot.text_size} : ayni_string_v1{}};
        const auto status = ayni_provider_post(slot.completion, &result);
        if (status == AYNI_OK) {
            std::memset(body(index), 0, slot.text_size); // No completed conversation cache in Mock.
            slot = {};
            --active;
        }
        return status;
    }
};

Provider::Provider() = default;
Provider::~Provider() {
    if (impl_ && impl_->active != 0) {
        static_cast<void>(impl_.release()); // close() reports the unresolved borrow to the owner.
    }
}
ayni_status_t Provider::configure(Config config) {
    if (impl_) {
        return AYNI_INVALID_STATE;
    }
    if ((config.variant != Variant::a && config.variant != Variant::b) ||
        (config.service != Service::text && config.service != Service::action) ||
        config.request_capacity == 0 || config.request_capacity > request_capacity_max ||
        config.text_max_bytes == 0 || config.text_max_bytes > text_bytes_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::unique_ptr<Impl> created(new (std::nothrow) Impl(config));
    if (!created) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    created->slots.reset(new (std::nothrow) Impl::Slot[config.request_capacity]);
    created->bodies.reset(new (std::nothrow) char[config.request_capacity * config.text_max_bytes]);
    if (!created->slots || !created->bodies) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    created->build_descriptor();
    impl_ = std::move(created);
    return AYNI_OK;
}
ayni_status_t Provider::describe(ayni_provider_v1 &out) const {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    out = impl_->descriptor;
    return AYNI_OK;
}
ayni_status_t Provider::advance() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    ayni_status_t result = AYNI_OK;
    for (uint32_t at = 0; at < impl_->config.request_capacity; ++at) {
        const auto status = impl_->advance(at);
        if (result == AYNI_OK && status != AYNI_OK) {
            result =
                status; // Preserve the first failure while giving other bounded slots progress.
        }
    }
    return result;
}
ayni_status_t Provider::close() {
    if (!impl_) {
        return AYNI_INVALID_STATE;
    }
    if (impl_->active != 0) {
        return AYNI_BUSY;
    }
    impl_.reset();
    return AYNI_OK;
}
uint32_t Provider::pending() const {
    return impl_ ? impl_->active : 0;
}
uint64_t Provider::executions() const {
    return impl_ ? impl_->executed : 0;
}
} // namespace ayni::providers::mock
