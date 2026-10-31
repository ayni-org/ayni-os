#include "tool_plan.h"
#include <array>
#include <ayni/providers/mock.h>
#include <ayni/values.hpp>
#include <ayni/workspace.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace ayni::providers::mock {
namespace {
constexpr uint32_t request_capacity_max = 64;
constexpr uint32_t text_bytes_max = 4096;
constexpr uint32_t fixture_input_max = 32768;
constexpr uint32_t fixture_reply_max = 512;
constexpr uint32_t model_envelope_bytes = 36;
constexpr uint32_t text_envelope_bytes = 24;
constexpr uint32_t fixture_envelope_bytes = 128;
// Bounded value framing, independent of body UTF-8 budget.
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
    char reply[fixture_reply_max]{}; // Copied fixture response; no borrowed config pointer.
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<char[]> bodies;
    std::unique_ptr<ToolPlanner> planner; // Only configured for the tools service.
    ayni_call_resource_v1 resource{};
    ayni_call_capability_v1 capability{};
    std::array<ayni_schema_node_v1, 2> input_schema{};
    ayni_schema_node_v1 result_schema{};
    ayni_business_contract_v1 business{};
    ayni_provider_v1 descriptor{};
    uint32_t active = 0;
    uint64_t executed = 0;

    char *body(uint32_t index) { return bodies.get() + index * config.text_max_bytes; }

    void build_descriptor() {
        const bool tools = config.service == Service::tools;
        const bool model = config.service == Service::text || tools;
        const bool health = config.service == Service::health;
        const bool a = config.variant == Variant::a;
        resource = {text(model    ? "model.slot"
                         : health ? "device.slot"
                                  : "presentation.slot"),
                    1, 0};
        capability = {};
        capability.struct_size = sizeof(capability);
        capability.version = contract_version;
        capability.capability_id = text(tools    ? "mock.tools"
                                        : model  ? "mock.text"
                                        : health ? "mock.health"
                                                 : "mock.present");
        capability.contract = {sizeof(capability.contract),
                               model || health ? AYNI_CALL_UNARY : AYNI_CALL_ACTION,
                               model ? AYNI_CALL_MODEL : AYNI_CALL_TOOL,
                               0,
                               model || health ? AYNI_CALL_IDEMPOTENT : AYNI_CALL_NOT_IDEMPOTENT,
                               AYNI_CALL_QUERY_NONE,
                               AYNI_CALL_CANCEL_REQUEST,
                               0,
                               request_timeout_ms,
                               AYNI_CALL_INPUT_VALUE,
                               tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
                               config.text_max_bytes +
                                   (config.business
                                        ? fixture_envelope_bytes
                                        : (model ? model_envelope_bytes : text_envelope_bytes)),
                               config.text_max_bytes,
                               1,
                               &resource,
                               nullptr};
        input_schema[0].kind = model ? AYNI_VALUE_OBJECT : AYNI_VALUE_TEXT;
        input_schema[0].subtree_end = model ? 2U : 1U;
        if (model) {
            input_schema[1].kind = AYNI_VALUE_TEXT;
            input_schema[1].field_id = AYNI_MODEL_BODY_FIELD_ID;
            input_schema[1].subtree_end = 2;
            input_schema[1].flags = AYNI_SCHEMA_REQUIRED;
            input_schema[1].name = text("body");
            input_schema[1].max_count = config.text_max_bytes;
        } else {
            input_schema[0].max_count = config.text_max_bytes;
        }
        result_schema.kind = AYNI_VALUE_TEXT;
        result_schema.subtree_end = 1;
        result_schema.max_count = config.text_max_bytes;
        business = {{input_schema.data(), model ? 2U : 1U}, {&result_schema, 1}, 0, 0, nullptr};
        capability.business = config.business ? config.business : &business;
        descriptor = {sizeof(descriptor),
                      contract_version,
                      text(tools    ? (a ? "mock.a.tools" : "mock.b.tools")
                           : model  ? (a ? "mock.a.text" : "mock.b.text")
                           : health ? "mock.health"
                                    : (a ? "mock.a.present" : "mock.b.present")),
                      text(tools    ? (a ? "mock.a.planner" : "mock.b.planner")
                           : model  ? (a ? "mock.a.model" : "mock.b.model")
                           : health ? "mock.device"
                                    : (a ? "mock.a.output" : "mock.b.output")),
                      {},
                      this,
                      1,
                      0,
                      &capability,
                      submit,
                      cancel,
                      nullptr,
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
            !same(request->endpoint_id, self.descriptor.endpoint_id)) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        ayni::ValueReader input;
        ayni_string_v1 body{};
        const bool model =
            self.config.service == Service::text || self.config.service == Service::tools;
        if (input.read(request->value) != AYNI_OK ||
            !ayni::ValueReader::text(
                model
                    ? input.field(AYNI_MODEL_BODY_FIELD_ID)
                    : (self.config.body_field ? input.field(self.config.body_field) : input.root()),
                body) ||
            body.size > self.config.text_max_bytes) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (uint32_t at = 0; at < self.config.request_capacity; ++at) {
            auto &slot = self.slots[at];
            if (slot.phase == Phase::empty) {
                if (body.size != 0) {
                    std::memcpy(self.body(at), body.data, body.size);
                }
                slot = {Phase::waiting, request->attempt, completion,
                        self.config.variant == Variant::a ? 1U : variant_b_ticks, body.size};
                if (self.planner) {
                    self.planner->prepare(at, *request, {self.body(at), body.size});
                }
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
                                                       {},
                                                       0,
                                                       0,
                                                       nullptr,
                                                       {},
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
        const bool tool_answer = success && planner;
        const auto answer = tool_answer ? planner->answer(index) : ToolAnswer{};
        ayni_provider_result_v1 result{sizeof(result),
                                       success ? AYNI_CALL_COMPLETED : AYNI_CALL_CANCELLED,
                                       slot.attempt,
                                       0,
                                       AYNI_OK,
                                       AYNI_REASON_NONE,
                                       success ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_NONE,
                                       0,
                                       success ? ayni_string_v1{body(index), slot.text_size}
                                               : ayni_string_v1{},
                                       0,
                                       0,
                                       nullptr,
                                       {},
                                       {}};
        if (tool_answer && answer.status != AYNI_OK) {
            // The model ran; its output could not become a typed answer.
            result.outcome = AYNI_CALL_FAILED;
            result.status = answer.status;
            result.reason = AYNI_REASON_MODEL_OUTPUT;
            result.effect = AYNI_CALL_EFFECT_NONE;
            result.result = {};
        } else if (tool_answer && answer.count != 0U) {
            result.result = {};
            result.tool_call_count = answer.count;
            result.tool_calls = answer.calls;
        } else if (success && config.reply_text.size) {
            result.result = {reply, config.reply_text.size};
        }
        const auto status = ayni_provider_post(slot.completion, &result);
        if (status == AYNI_OK) {
            if (planner) {
                planner->clear(index);
            }
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
        (config.service != Service::text && config.service != Service::action &&
         config.service != Service::tools && config.service != Service::health) ||
        config.request_capacity == 0 || config.request_capacity > request_capacity_max ||
        config.text_max_bytes == 0 ||
        config.text_max_bytes > (config.reply_text.size ? fixture_input_max : text_bytes_max) ||
        config.reply_text.size > fixture_reply_max ||
        config.reply_text.size > config.text_max_bytes ||
        (config.reply_text.size && !config.reply_text.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!config.business && config.body_field) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (config.business) {
        ayni::Workspace workspace;
        const auto memory = workspace.create(ayni::Workspace::Purpose::value);
        if (memory != AYNI_OK) {
            return memory;
        }
        const auto &business = *config.business;
        if (ayni_schema_validate(workspace.get(), &business.input) != AYNI_OK ||
            ayni_schema_validate(workspace.get(), &business.result) != AYNI_OK ||
            business.reserved || business.projection_count > AYNI_AUTHORITY_FIELD_COUNT_MAX ||
            ((business.projection_count != 0) != (business.projections != nullptr))) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (config.service == Service::text || config.service == Service::tools) {
            return AYNI_UNSUPPORTED;
        }
        const ayni_schema_node_v1 *body_schema = nullptr;
        if (!config.body_field && business.input.nodes[0].kind == AYNI_VALUE_TEXT) {
            body_schema = business.input.nodes;
        } else if (config.body_field && business.input.nodes[0].kind == AYNI_VALUE_OBJECT) {
            for (uint32_t at = 1; at < business.input.node_count;
                 at = business.input.nodes[at].subtree_end) {
                if (business.input.nodes[at].field_id == config.body_field) {
                    body_schema = &business.input.nodes[at];
                    break;
                }
            }
        }
        const auto &result = business.result.nodes[0];
        if (!body_schema || body_schema->kind != AYNI_VALUE_TEXT ||
            body_schema->max_count > config.text_max_bytes || result.kind != AYNI_VALUE_TEXT ||
            result.max_count < body_schema->max_count ||
            result.min_count > body_schema->min_count ||
            config.reply_text.size > result.max_count ||
            (config.reply_text.size && config.reply_text.size < result.min_count)) {
            return AYNI_UNSUPPORTED;
        }
    }
    std::unique_ptr<Impl> created(new (std::nothrow) Impl(config));
    if (!created) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (config.reply_text.size) {
        std::memcpy(created->reply, config.reply_text.data, config.reply_text.size);
        created->config.reply_text.data = created->reply;
    }
    created->slots.reset(new (std::nothrow) Impl::Slot[config.request_capacity]);
    created->bodies.reset(new (std::nothrow) char[config.request_capacity * config.text_max_bytes]);
    if (config.service == Service::tools) {
        created->planner.reset(new (std::nothrow) ToolPlanner);
        if (!created->planner) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const auto status = created->planner->configure(
            config.request_capacity, config.text_max_bytes, config.text_max_bytes);
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (!created->slots || !created->bodies ||
        (config.service == Service::tools && !created->planner)) {
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
void Provider::abandon_kernel() {
    if (!impl_) {
        return;
    }
    for (uint32_t at = 0; at < impl_->config.request_capacity; ++at) {
        impl_->slots[at] = {};
        std::memset(impl_->body(at), 0, impl_->config.text_max_bytes);
        if (impl_->planner) {
            impl_->planner->clear(at);
        }
    }
    impl_->active = 0;
}
uint32_t Provider::pending() const {
    return impl_ ? impl_->active : 0;
}
uint64_t Provider::executions() const {
    return impl_ ? impl_->executed : 0;
}
} // namespace ayni::providers::mock
