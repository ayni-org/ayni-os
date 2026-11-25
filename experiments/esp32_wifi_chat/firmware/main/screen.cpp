#include "chat.h"
#include "wake.h"
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
namespace wifichat {
namespace {
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
constexpr uint32_t version = 1, text_field = 1, render_timeout_ms = 30000;
} // namespace
// The whole text field at its limit, measured from the contract's own shape (ADR-0095).
uint32_t screen_input_bytes() {
    return ayni::text_value_bytes(answer_bytes, text_field);
}
// The screen is a TOOL capability. The selected renderer is only its raster backend;
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
                           screen_input_bytes(), 128, 1, &screen_resource_, nullptr,
                           AYNI_CALL_DURABILITY_EPHEMERAL, 0},
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
        text("Display a UTF-8 document on the connected screen."),
        text("Input is an object with required UTF-8 text, at most 4096 bytes. Empty text clears "
             "the page."),
        text("Success confirms the first page was painted by LCD DMA; pages are navigable in the "
             "touchscreen. It does not prove visual correctness."),
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
    if (!self.announced_ || self.closing_ != Closing::open || !request || !completion ||
        self.screen_ != ScreenState::idle) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    ayni_string_v1 body{};
    if (reader.read(request->value) != AYNI_OK ||
        !ayni::ValueReader::text(reader.field(text_field), body)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    // Publish the authorized identity before a same-device backend can consume
    // the request on another CPU. Failed enqueue releases that reservation.
    self.transport_.allow_display(request->logical_call_id);
    if (!self.transport_.send(Kind::render_text, request->logical_call_id, body.data, body.size)) {
        self.transport_.allow_display(0);
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.screen_completion_ = completion;
    self.screen_attempt_ = request->attempt;
    self.screen_call_ = request->logical_call_id;
    self.screen_deadline_ = request->deadline_us;
    self.screen_pages_ = 0;
    self.screen_ = ScreenState::waiting;
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
        wake_owner(); // Inside ayni_step: the next tick posts the result without waiting.
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
} // namespace wifichat
