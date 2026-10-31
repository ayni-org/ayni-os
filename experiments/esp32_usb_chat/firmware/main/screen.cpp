#include "chat.h"
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
namespace usbchat {
namespace {
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
constexpr uint32_t version = 1, text_field = 1, render_timeout_ms = 30000;
} // namespace
// The screen is a TOOL capability. The Mac is only its font/raster backend;
// completion requires this device's actual LCD DMA completion, never a host claim.
ayni_status_t Chat::configure_screen() {
    const ayni_capability_desc_v1 authority{
        sizeof(authority), version, text("screen.text"), 0, 0, nullptr};
    auto rc = ayni_register_capability(kernel_, &authority);
    screen_input_[0].kind = AYNI_VALUE_OBJECT;
    screen_input_[0].subtree_end = 2;
    auto &body = screen_input_[1];
    body.kind = AYNI_VALUE_TEXT;
    body.field_id = text_field;
    body.subtree_end = 2;
    body.flags = AYNI_SCHEMA_REQUIRED;
    body.name = text("text");
    body.max_count = answer_bytes;
    screen_result_.kind = AYNI_VALUE_TEXT;
    screen_result_.subtree_end = 1;
    screen_result_.max_count = 128;
    screen_business_ = {{screen_input_, 2}, {&screen_result_, 1}, 0, 0, nullptr};
    screen_resource_ = {text("screen.slot"), 1, 0};
    screen_capability_ = {sizeof(screen_capability_),
                          version,
                          text("screen.text"),
                          {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                           AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                           render_timeout_ms, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT,
                           answer_bytes + 64, 128, 1, &screen_resource_, nullptr},
                          &screen_business_};
    if (rc == AYNI_OK) {
        rc = ayni_register_call_capability(kernel_, &screen_capability_);
    }
    screen_provider_ = {sizeof(screen_provider_),
                        version,
                        text("screen.provider"),
                        text("screen.lcd"),
                        {},
                        this,
                        1,
                        0,
                        &screen_capability_,
                        submit_screen,
                        cancel_screen,
                        nullptr,
                        nullptr};
    if (rc == AYNI_OK) {
        rc = ayni_register_provider(kernel_, &screen_provider_);
    }
    const ayni_resource_desc_v1 resource{
        sizeof(resource), AYNI_RESOURCE_EXCLUSIVE, text("screen.slot"), 1, 0, 0, 0, nullptr, {}};
    if (rc == AYNI_OK) {
        rc = ayni_register_resource(kernel_, &resource);
    }
    if (rc == AYNI_OK) {
        rc = ayni_set_resource_health(kernel_, resource.resource_id, AYNI_RESOURCE_ONLINE);
    }
    const ayni_binding_v1 binding{sizeof(binding),
                                  version,
                                  text("screen.show_text"),
                                  text("screen.text"),
                                  version,
                                  version,
                                  screen_provider_.provider_id,
                                  screen_provider_.endpoint_id,
                                  {},
                                  0,
                                  0,
                                  nullptr};
    if (rc == AYNI_OK) {
        rc = ayni_register_binding(kernel_, &binding);
    }
    const ayni_tool_usage_v1 usage{
        sizeof(usage),
        version,
        binding.binding_id,
        version,
        text("screen.text"),
        text("Display the answer on the connected ESP32 screen. Call this for every user-facing "
             "answer."),
        text("Input is an object with required UTF-8 text, at most 4096 bytes. Empty text clears "
             "the page."),
        text("Success confirms the first page was painted by LCD DMA; pages are navigable in the "
             "Mac window. It does not prove visual correctness."),
        text("{\"text\":\"你好，世界！\"}"),
        0,
        0,
        nullptr};
    return rc == AYNI_OK ? ayni_register_capability_usage(
                               kernel_, text("Show a UTF-8 reply on the ESP32 LCD"), &usage)
                         : rc;
}
ayni_submit_result_t Chat::submit_screen(void *ctx, const ayni_provider_request_v1 *request,
                                         ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Chat *>(ctx);
    if (!request || !completion || self.screen_ != ScreenState::idle) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    ayni_string_v1 body{};
    if (reader.read(request->value) != AYNI_OK ||
        !ayni::ValueReader::text(reader.field(text_field), body)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (!self.transport_.send(Kind::render_text, request->logical_call_id, body.data, body.size)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.screen_completion_ = completion;
    self.screen_attempt_ = request->attempt;
    self.screen_call_ = request->logical_call_id;
    self.screen_deadline_ = request->deadline_us;
    self.screen_pages_ = 0;
    self.screen_ = ScreenState::waiting;
    self.transport_.allow_display(self.screen_call_);
    self.status(self.turn_, "screen capability dispatched");
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t Chat::cancel_screen(void *ctx, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Chat *>(ctx);
    if (self.screen_ == ScreenState::idle ||
        attempt.boot_epoch != self.screen_attempt_.boot_epoch ||
        attempt.request_id != self.screen_attempt_.request_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (self.screen_ == ScreenState::waiting) {
        self.screen_ = ScreenState::failed;
    }
    return AYNI_SUBMIT_ACCEPTED;
}
void Chat::publish_screen() {
    if (screen_ == ScreenState::waiting && uint64_t(esp_timer_get_time()) >= screen_deadline_) {
        screen_ = ScreenState::failed;
    }
    if (screen_ == ScreenState::waiting || screen_ == ScreenState::idle) {
        return;
    }
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = screen_attempt_;
    char facts[128];
    if (screen_ == ScreenState::painted) {
        std::snprintf(facts, sizeof(facts),
                      "{\"displayed\":true,\"pages\":%lu,\"confirmed\":\"device-lcd-dma\"}",
                      static_cast<unsigned long>(screen_pages_));
        result.outcome = AYNI_CALL_COMPLETED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        result.result = text(facts);
    } else {
        result.outcome = AYNI_CALL_ABANDONED;
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
        transport_.allow_display(0);
    }
    if (ayni_provider_post(screen_completion_, &result) == AYNI_OK) {
        screen_ = ScreenState::idle;
        screen_completion_ = nullptr;
        status(turn_, result.outcome == AYNI_CALL_COMPLETED ? "screen capability completed"
                                                            : "screen outcome unknown");
    }
}
// The Mac preserves JSON numeric lexemes and serializes lossless preorder nodes.
// Strings borrow this fixed lane only until provider_post has copied the proposal.
bool Chat::decode_tool(const Packet &p) {
    if (p.size < 8 || p.size > tool_wire_.size()) {
        return false;
    }
    std::memcpy(tool_wire_.data(), p.body.data(), p.size);
    auto *data = tool_wire_.data();
    size_t pos = 8;
    const auto u16 = [](const uint8_t *v) { return uint32_t(v[0]) | (uint32_t(v[1]) << 8U); };
    const uint32_t id_bytes = u16(data), name_bytes = u16(data + 2), nodes = u16(data + 4);
    if (!id_bytes || id_bytes > AYNI_MODEL_TOOL_ID_BYTES_MAX || !name_bytes ||
        name_bytes > AYNI_CATALOG_ID_MAX_BYTES || !nodes || nodes > argument_nodes_.size() ||
        data[6] || data[7]) {
        return false;
    }
    auto take = [&](size_t bytes, ayni_string_v1 &out) {
        if (bytes > p.size - pos) {
            return false;
        }
        out = {reinterpret_cast<char *>(data + pos), uint32_t(bytes)};
        pos += bytes;
        return true;
    };
    proposal_ = {};
    proposal_.struct_size = sizeof(proposal_);
    proposal_.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
    if (!take(id_bytes, proposal_.protocol_id) || !take(name_bytes, proposal_.function_name)) {
        return false;
    }
    for (uint32_t at = 0; at < nodes; ++at) {
        if (p.size - pos < 12) {
            return false;
        }
        auto &node = argument_nodes_[at];
        node = {};
        node.kind = get32(data + pos);
        node.subtree_end = get32(data + pos + 4);
        const auto name = u16(data + pos + 8), value = u16(data + pos + 10);
        pos += 12;
        ayni_string_v1 raw{};
        if (!take(name, node.name) || !take(value, raw)) {
            return false;
        }
        if (node.kind == AYNI_ARGUMENT_BOOL) {
            if (raw.size != 1 || (raw.data[0] != '0' && raw.data[0] != '1')) {
                return false;
            }
            node.value.boolean = raw.data[0] == '1';
        } else {
            node.value.text = raw;
        }
    }
    proposal_.input.arguments = {argument_nodes_.data(), nodes};
    return pos == p.size && ayni_arguments_validate(&proposal_.input.arguments) == AYNI_OK;
}
} // namespace usbchat
