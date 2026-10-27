#include <ayni/providers/mock.h>

#include <cstring>
#include <new>
#include <utility>

namespace ayni::providers::mock {
namespace {
constexpr uint32_t request_capacity_max = 64;
constexpr uint32_t text_bytes_max = 4096;
constexpr uint32_t fixture_input_max = 8192;
constexpr uint32_t fixture_reply_max = 512;
constexpr uint32_t contract_version = 1;
constexpr uint32_t request_timeout_ms = 30000;
constexpr uint32_t variant_b_ticks = 2;
constexpr char call_prefix[] = "call:";
constexpr uint32_t call_prefix_size = sizeof(call_prefix) - 1U;
// Tool-call list format 1 (contracts/storage/records.md), as a Provider sizes it.
constexpr uint32_t list_header_size = 4;
constexpr uint32_t call_fixed_size = 16;
constexpr uint32_t field_fixed_size = 12;
constexpr uint32_t integer_size = 8;
constexpr uint32_t token_length_size = 4;
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
    // One field of a planned call, copied from the offered tool at submit.
    struct Field {
        uint32_t id;
        ayni_authority_value_kind_t kind;
        int64_t integer;
        uint32_t unit_size;
        uint32_t token_size;
        char unit[AYNI_CATALOG_ID_MAX_BYTES];
        char token[AYNI_CATALOG_ID_MAX_BYTES];
    };
    struct Call {
        uint32_t version;
        uint32_t id_size;
        uint32_t text_limit;
        uint32_t field_count;
        char id[AYNI_CATALOG_ID_MAX_BYTES];
        Field fields[AYNI_AUTHORITY_FIELD_COUNT_MAX];
    };
    // The tools service's decided answer for one slot; status OK or the model-output failure.
    struct Plan {
        ayni_status_t status;
        uint32_t count;
        uint32_t body_at; // Offset of the shared call body in the slot's text.
        Call calls[AYNI_CALL_TOOL_COUNT_MAX];
    };
    explicit Impl(Config config_value) : config(config_value) {}

    Config config;
    char reply[fixture_reply_max]{}; // Copied fixture response; no borrowed config pointer.
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<char[]> bodies;
    std::unique_ptr<Plan[]> plans; // Tools service only.
    // Owner-thread scratch for one tool view at submit and one posted list.
    ayni_tool_v1 tool{};
    ayni_tool_call_v1 posted[AYNI_CALL_TOOL_COUNT_MAX]{};
    ayni_scope_field_v1 posted_fields[AYNI_CALL_TOOL_COUNT_MAX][AYNI_AUTHORITY_FIELD_COUNT_MAX]{};
    ayni_string_v1 posted_tokens[AYNI_CALL_TOOL_COUNT_MAX][AYNI_AUTHORITY_FIELD_COUNT_MAX]{};
    ayni_call_resource_v1 resource{};
    ayni_call_capability_v1 capability{};
    ayni_provider_v1 descriptor{};
    uint32_t active = 0;
    uint64_t executed = 0;

    char *body(uint32_t index) { return bodies.get() + index * config.text_max_bytes; }

    void build_descriptor() {
        const bool tools = config.service == Service::tools;
        const bool model = config.service == Service::text || tools;
        const bool a = config.variant == Variant::a;
        resource = {text(model ? "model.slot" : "presentation.slot"), 1, 0};
        capability = {};
        capability.struct_size = sizeof(capability);
        capability.version = contract_version;
        capability.capability_id = text(tools   ? "mock.tools"
                                        : model ? "mock.text"
                                                : "mock.present");
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
                               tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
                               config.text_max_bytes,
                               config.text_max_bytes,
                               1,
                               &resource,
                               nullptr};
        descriptor = {sizeof(descriptor),
                      contract_version,
                      text(tools   ? (a ? "mock.a.tools" : "mock.b.tools")
                           : model ? (a ? "mock.a.text" : "mock.b.text")
                                   : (a ? "mock.a.present" : "mock.b.present")),
                      text(tools   ? (a ? "mock.a.planner" : "mock.b.planner")
                           : model ? (a ? "mock.a.model" : "mock.b.model")
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
            !same(request->endpoint_id, self.descriptor.endpoint_id) ||
            (request->arguments.field_count != 0 && request->arguments.fields == nullptr) ||
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
                if (self.plans) {
                    self.plan(*request, self.plans[at]); // Tool views live only during submit.
                }
                ++self.active;
                return AYNI_SUBMIT_ACCEPTED;
            }
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }

    // Copies what the answer needs from the offered tools: each leading
    // "call:<id>" word must name one, and the rest of the text is every body.
    void plan(const ayni_provider_request_v1 &request, Plan &out) {
        out = {};
        out.status = AYNI_OK;
        const char *data = request.text.data;
        const uint32_t size = request.text.size;
        uint32_t at = 0;
        while (out.count < AYNI_CALL_TOOL_COUNT_MAX && size - at >= call_prefix_size &&
               std::memcmp(data + at, call_prefix, call_prefix_size) == 0) {
            uint32_t end = at + call_prefix_size;
            while (end < size && data[end] != ' ') {
                ++end;
            }
            const ayni_string_v1 name{data + at + call_prefix_size, end - at - call_prefix_size};
            if (!copy_tool(request, name, out.calls[out.count])) {
                out.status = AYNI_INVALID_ARGUMENT;
                return;
            }
            ++out.count;
            at = end < size ? end + 1U : end;
        }
        out.body_at = at;
        uint32_t list = list_header_size;
        for (uint32_t call = 0; call < out.count; ++call) {
            const auto &planned = out.calls[call];
            if (size - at > planned.text_limit) {
                out.status = AYNI_INVALID_ARGUMENT;
                return;
            }
            list += call_fixed_size + planned.id_size + (size - at);
            for (uint32_t field = 0; field < planned.field_count; ++field) {
                const auto &value = planned.fields[field];
                list += field_fixed_size + value.unit_size +
                        (value.kind == AYNI_AUTHORITY_I64 ? integer_size
                                                          : token_length_size + value.token_size);
            }
        }
        if (out.count != 0U && list > capability.contract.result_text_limit) {
            out.status = AYNI_CAPACITY_EXCEEDED;
        }
    }

    bool copy_tool(const ayni_provider_request_v1 &request, ayni_string_v1 name, Call &out) {
        for (uint32_t index = 0; index < request.tool_count; ++index) {
            tool = {};
            tool.struct_size = sizeof(tool);
            if (request.tool(request.tool_context, index, &tool) != AYNI_OK ||
                !same(tool.binding_id, name)) {
                continue;
            }
            out = {};
            out.version = tool.binding_version;
            out.id_size = tool.binding_id.size;
            out.text_limit = tool.text_limit;
            out.field_count = tool.field_count;
            std::memcpy(out.id, tool.binding_id.data, tool.binding_id.size);
            for (uint32_t at = 0; at < tool.field_count; ++at) {
                const auto &domain = tool.fields[at].domain;
                auto &field = out.fields[at];
                field.id = tool.fields[at].field_id;
                field.kind = domain.kind;
                field.unit_size = domain.unit.size;
                if (domain.unit.size != 0) {
                    std::memcpy(field.unit, domain.unit.data, domain.unit.size);
                }
                if (domain.kind == AYNI_AUTHORITY_I64) {
                    field.integer = domain.values.integer.min_inclusive;
                } else {
                    const auto token = domain.values.tokens.members[0];
                    field.token_size = token.size;
                    std::memcpy(field.token, token.data, token.size);
                }
            }
            return true;
        }
        return false;
    }

    // Views of a slot's planned calls in the scratch above, for one post.
    const ayni_tool_call_v1 *calls_of(uint32_t index) {
        const auto &plan = plans[index];
        const ayni_string_v1 body_text{body(index) + plan.body_at,
                                       slots[index].text_size - plan.body_at};
        for (uint32_t call = 0; call < plan.count; ++call) {
            const auto &planned = plan.calls[call];
            for (uint32_t at = 0; at < planned.field_count; ++at) {
                const auto &field = planned.fields[at];
                auto &out = posted_fields[call][at];
                out = {};
                out.struct_size = sizeof(out);
                out.field_id = field.id;
                out.allowed.struct_size = sizeof(out.allowed);
                out.allowed.kind = field.kind;
                out.allowed.unit = {field.unit, field.unit_size};
                if (field.kind == AYNI_AUTHORITY_I64) {
                    out.allowed.values.integer = {field.integer, field.integer};
                } else {
                    posted_tokens[call][at] = {field.token, field.token_size};
                    out.allowed.values.tokens = {&posted_tokens[call][at], 1, 0};
                }
            }
            posted[call] = {sizeof(ayni_tool_call_v1),
                            planned.version,
                            {planned.id, planned.id_size},
                            {sizeof(ayni_scope_v1), planned.field_count, posted_fields[call]},
                            body_text};
        }
        return posted;
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
                                                       nullptr};
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
        const Plan *plan = success && plans ? &plans[index] : nullptr;
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
                                       nullptr};
        if (plan != nullptr && plan->status != AYNI_OK) {
            // The model ran; its output could not become a typed answer.
            result.outcome = AYNI_CALL_FAILED;
            result.status = plan->status;
            result.reason = AYNI_REASON_MODEL_OUTPUT;
            result.effect = AYNI_CALL_EFFECT_NONE;
            result.result = {};
        } else if (plan != nullptr && plan->count != 0U) {
            result.result = {};
            result.tool_call_count = plan->count;
            result.tool_calls = calls_of(index);
        } else if (success && config.reply_text.size) {
            result.result = {reply, config.reply_text.size};
        }
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
        (config.service != Service::text && config.service != Service::action &&
         config.service != Service::tools) ||
        config.request_capacity == 0 || config.request_capacity > request_capacity_max ||
        config.text_max_bytes == 0 ||
        config.text_max_bytes > (config.reply_text.size ? fixture_input_max : text_bytes_max) ||
        config.reply_text.size > fixture_reply_max ||
        config.reply_text.size > config.text_max_bytes ||
        (config.reply_text.size && !config.reply_text.data)) {
        return AYNI_INVALID_ARGUMENT;
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
        created->plans.reset(new (std::nothrow) Impl::Plan[config.request_capacity]);
    }
    if (!created->slots || !created->bodies ||
        (config.service == Service::tools && !created->plans)) {
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
