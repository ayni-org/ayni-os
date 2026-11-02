#include <ayni/model_messages.hpp>
#include <ayni/providers/fixture_model.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
namespace ayni::providers::fixture_model {
namespace {
constexpr uint32_t version = 1;
constexpr uint32_t timeout_ms = 30000;
constexpr uint32_t envelope_nodes = 4;
constexpr uint32_t protocol_id_bytes = 40;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
bool same(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && (!a.size || std::memcmp(a.data, b.data, a.size) == 0);
}
} // namespace
uint32_t call_input_bytes(bool bodies) {
    if (!bodies) {
        return text_value_bytes(prompt_bytes_max, AYNI_MODEL_BODY_FIELD_ID);
    }
    ValueBuilder<2> shape;
    shape.object();
    shape.body(AYNI_MODEL_BODY_FIELD_ID, {1, 0, 0, 0});
    shape.end();
    ayni_value_v1 value{};
    return shape.finish(value) == AYNI_OK ? encoded_bytes(value) : 0U;
}
Provider::Provider(Variant variant, Service service, void *context, uint64_t (*now)(void *),
                   const ayni_body_store_v1 *bodies)
    : variant_(variant), service_(service), clock_context_(context), now_us_(now), bodies_(bodies) {
    const bool tools = service == Service::tools;
    resource_ = {text(tools ? "model.slot" : "summary.slot"), 1, 0};
    input_[0].kind = AYNI_VALUE_OBJECT;
    input_[0].subtree_end = static_cast<uint32_t>(input_.size());
    input_[1].kind = AYNI_VALUE_TEXT;
    input_[1].field_id = AYNI_MODEL_BODY_FIELD_ID;
    input_[1].flags = AYNI_SCHEMA_REQUIRED | (bodies ? AYNI_SCHEMA_EXTERNAL : 0U);
    input_[1].subtree_end = static_cast<uint32_t>(input_.size());
    input_[1].name = text("body");
    input_[1].max_count = prompt_bytes_max;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = call_result_bytes;
    business_ = {
        {input_.data(), static_cast<uint32_t>(input_.size())}, {&result_, 1}, 0, 0, nullptr};
    capability_ = {sizeof(capability_),
                   version,
                   text(tools ? "fixture.tools" : "fixture.summary"),
                   {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL, 0,
                    AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                    timeout_ms, AYNI_CALL_INPUT_VALUE,
                    tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
                    call_input_bytes(bodies != nullptr), call_result_bytes, 1, &resource_, nullptr},
                   &business_};
    descriptor_ = {
        sizeof(descriptor_),
        version,
        text(tools ? (variant == Variant::a ? "fixture.a" : "fixture.b") : "fixture.summary"),
        text(tools ? (variant == Variant::a ? "fixture.a.model" : "fixture.b.model")
                   : "fixture.summary.model"),
        {},
        this,
        1,
        0,
        &capability_,
        submit,
        cancel,
        nullptr,
        nullptr};
}
ayni_status_t Provider::describe(ayni_provider_v1 &out) const {
    if (!now_us_ || (variant_ != Variant::a && variant_ != Variant::b) ||
        (service_ != Service::tools && service_ != Service::summary)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = descriptor_;
    return AYNI_OK;
}
// C-CALL borrow: parse only during submit and copy the choice. No transient input survives.
ayni_submit_result_t Provider::submit(void *context, const ayni_provider_request_v1 *request,
                                      ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Provider *>(context);
    if (!request || !completion || self.pending_ || !self.now_us_ ||
        request->struct_size != sizeof(*request) || request->capability_version != version ||
        !request->attempt.boot_epoch || !request->attempt.request_id || !request->logical_call_id ||
        !request->operation_id || !request->deadline_us ||
        !same(request->capability_id, self.capability_.capability_id) ||
        !same(request->endpoint_id, self.descriptor_.endpoint_id)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ValueReader value;
    ayni_string_v1 body{};
    ayni_body_ref_v1 ref{};
    if (value.read(request->value) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto *field = value.field(AYNI_MODEL_BODY_FIELD_ID);
    struct Erase { // No referenced prompt survives submit.
        std::array<char, prompt_bytes_max> &prompt;
        uint32_t size = 0;
        ~Erase() { std::memset(prompt.data(), 0, size); }
    } erase{self.prompt_};
    if (self.bodies_ && ValueReader::body(field, ref) && ref.size <= self.prompt_.size()) {
        auto *out = reinterpret_cast<uint8_t *>(self.prompt_.data());
        erase.size = ref.size;
        if (self.bodies_->read(self.bodies_->context, &ref, out, ref.size) != AYNI_OK ||
            ayni_body_verify(&ref, {out, ref.size}) != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        body = {self.prompt_.data(), ref.size};
    } else if (!ValueReader::text(field, body)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_string_v1 instructions{}, messages{};
    if (model::split_instructions(body, instructions, messages) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    Answer answer = Answer::final;
    if (self.service_ == Service::tools) {
        model::Reader reader(messages);
        model::Message message;
        constexpr uint32_t many_sums = 8;
        uint32_t requested = 0, completed = 0;
        while (reader.next(message)) {
            if (message.role == model::Role::user) {
                requested = same(message.text, text("sum"))         ? 1U
                            : same(message.text, text("many sums")) ? many_sums
                                                                    : 0U;
                completed = 0;
            } else if (message.role == model::Role::tool) {
                if (completed < requested) {
                    ++completed;
                }
            } else if (message.role == model::Role::assistant) {
                requested = 0;
            }
        }
        answer = completed < requested ? Answer::sum : Answer::final;
        if (!reader.finished()) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
    }
    ++self.submissions_;
    self.pending_ = Pending{answer, request->attempt, request->logical_call_id,
                            request->deadline_us, completion};
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t Provider::cancel(void *context, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Provider *>(context);
    if (!self.pending_ || self.pending_->attempt.boot_epoch != attempt.boot_epoch ||
        self.pending_->attempt.request_id != attempt.request_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.pending_->answer = Answer::cancelled;
    return AYNI_SUBMIT_ACCEPTED;
}
// Publication constructs only a four-node generic proposal. Core remains the sole
// business admission/authority/dispatch owner; this fixture never computes the sum.
ayni_status_t Provider::advance() {
    if (!pending_) {
        return AYNI_OK;
    }
    const auto &pending = *pending_;
    const bool cancelled = pending.answer == Answer::cancelled;
    const bool expired = now_us_(clock_context_) >= pending.deadline_us;
    auto result = ayni_provider_result_v1{};
    result.struct_size = sizeof(result);
    result.attempt = pending.attempt;
    result.outcome = cancelled ? AYNI_CALL_CANCELLED
                     : expired ? AYNI_CALL_FAILED
                               : AYNI_CALL_COMPLETED;
    result.status = expired && !cancelled ? AYNI_EXPIRED : AYNI_OK;
    result.effect = cancelled || expired ? AYNI_CALL_EFFECT_NONE : AYNI_CALL_EFFECT_COMPLETE;
    std::array<ayni_argument_node_v1, envelope_nodes> nodes{};
    ayni_tool_call_v1 call{};
    char id[protocol_id_bytes]{};
    if (!cancelled && !expired && pending.answer == Answer::sum) {
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = envelope_nodes;
        nodes[1].kind = AYNI_ARGUMENT_STRING;
        nodes[1].subtree_end = 2;
        nodes[1].name = text("binding_id");
        nodes[1].value.text = text("software.sum");
        nodes[2].kind = AYNI_ARGUMENT_NUMBER;
        nodes[2].subtree_end = 3;
        nodes[2].name = text("binding_version");
        nodes[2].value.text = text("1");
        nodes[3].kind = AYNI_ARGUMENT_NUMBER;
        nodes[3].subtree_end = envelope_nodes;
        nodes[3].name = text("arguments");
        nodes[3].value.text = text("512");
        const int size = std::snprintf(id, sizeof(id), "fixture_%llu",
                                       static_cast<unsigned long long>(pending.logical_call));
        if (size <= 0 || static_cast<size_t>(size) >= sizeof(id)) {
            return AYNI_INTERNAL;
        }
        call.struct_size = sizeof(call);
        call.function_name = text("ayni_call_capability");
        call.protocol_id = {id, static_cast<uint32_t>(size)};
        call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        call.input.arguments = {nodes.data(), static_cast<uint32_t>(nodes.size())};
        result.tool_call_count = 1;
        result.tool_calls = &call;
    } else if (!cancelled && !expired) {
        result.result = text(service_ == Service::summary ? "Fixture summary."
                             : variant_ == Variant::a     ? "A: fixture reply."
                                                          : "B: fixture reply.");
    }
    const auto status = ayni_provider_post(pending.completion, &result);
    if (status == AYNI_OK) {
        if (!cancelled && !expired) {
            ++executions_;
        }
        pending_.reset();
    }
    return status;
}
} // namespace ayni::providers::fixture_model
