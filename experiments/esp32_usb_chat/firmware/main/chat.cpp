#include "chat.h"
#include "configuration.h"
#include "motor_contract.h"
#include "wake.h"
#include <algorithm>
#include <ayni/bodies.h>
#include <ayni/host_authority.h>
#include <ayni/model_messages.hpp>
#include <ayni/resources.h>
#include <ayni/scenes.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <new>
namespace usbchat {
namespace {
constexpr uint32_t version = 1, step_units = 32, step_us = 4000;
constexpr uint64_t principal = 1, command_range = UINT64_C(1) << 32;
// Boot-local motion authority also expires after one day; no silent renewal.
constexpr uint64_t motor_grant_us = UINT64_C(24) * 60 * 60 * 1000000;
constexpr char model_capability[] = "usb.model.text", summary_capability[] = "usb.model.summary",
               model_binding[] = "usb.model", summary_binding[] = "usb.summary",
               strategy_id[] = "usb.chat", motion_capability[] = "motor.run";
constexpr const char *grant_capabilities[] = {model_capability, summary_capability, "screen.text",
                                              "speaker.tone",   "speaker.speech",   "motor.status",
                                              "motor.plan",     "motor.run",        "motor.stop"};
constexpr uint64_t grant_count = sizeof(grant_capabilities) / sizeof(grant_capabilities[0]);
// Trusted deployment instructions, framed apart from the conversation data (ADR-0074). The
// ToolLoop states the generic rules of the four functions after them.
constexpr char instructions[] =
    "你是运行在 ESP32-S3 上的 Ayni 助手。你必须调用系统的屏幕能力来显示给用户的完整回复。"
    "先 ayni_list_capabilities，再 ayni_inspect_capability 查看精确绑定与用法，"
    "最后用 ayni_call_capability 调用屏幕能力，arguments 中包含完整回复 text。"
    "每轮最多提议 8 个工具调用，系统按提议顺序执行并返回真实工具结果；成功后用简短文字确认。"
    "只有用户要求声音时，才发现并调用 speaker.play_tone 提示音或 speaker.speak_text 中文朗读。"
    "扬声器默认音量 80%，可按用户要求指定 volume_percent；朗读内容应简短、最多 30 秒。"
    "电机能力 motor.status 查询、motor.plan 仅计划；用户要求转动时发现并调用 motor.run。"
    "motor.run 的两个百分比为 PWM 占空比 -90..90，零表示对应电机不动，时长 50..5000ms "
    "且为5的倍数；每次实际调用都必须等待 Mac 人工批准；结果只证明控制器输出结束，"
    "不证明轮速或距离。不要把 plan 当作已转动。"
    "只能人工架空台架实验；不支持无人运动，停止链尚未完成硬件验证。"
    "只有用户明确要求才可转动；速度与时间不得超出用户要求。两只分别测试应逐次调用，另一只设零。"
    "拒绝、超时、取消或结果未知的动作不得自动重试。不要宣称达到实测转速、距离或角度。"
    "如需停止可调用 motor.stop；该工具成功只代表停止请求已提交。"
    "没有 shell，不要调用。屏幕中文正文 UTF-8 总长最多 4096 字节，不超过 1200 个汉字。";
template <class T> T sized() {
    T out{};
    out.struct_size = sizeof(out);
    return out;
}
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
ayni_scope_v1 scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && !std::memcmp(a.data, b, a.size);
}
} // namespace
// The model Call carries the destination and only a reference to its prompt (ADR-0096).
uint32_t model_input_bytes(ayni_string_v1 destination) {
    ayni::ValueBuilder<3> shape;
    shape.object();
    shape.identifier(AYNI_MODEL_DESTINATION_FIELD_ID, destination);
    shape.body(AYNI_MODEL_BODY_FIELD_ID, {1, 0, 0, 0});
    shape.end();
    ayni_value_v1 value{};
    return shape.finish(value) == AYNI_OK ? ayni::encoded_bytes(value) : 0U;
}
void Chat::status(uint64_t id, const char *message) {
    transport_.send(Kind::status, id, message, std::strlen(message));
}
ayni_status_t Chat::initialize(const char *route) {
    if (!route || std::strlen(route) != 16 || kernel_) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::snprintf(route_, sizeof(route_), "model.%s", route);
    destination_ = text(route_);
    void *host_memory =
        heap_caps_malloc(sizeof(ayni::hosts::esp_idf::Host), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!host_memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    host_ = new (host_memory) ayni::hosts::esp_idf::Host;
    const auto opened = host_->open_storage("ayni", 4, true);
    if (opened.status != ayni::hosts::esp_idf::StorageStatus::ok) {
        char error[96];
        std::snprintf(error, sizeof(error), "storage open=%u cause=%d", unsigned(opened.status),
                      opened.error);
        status(0, error);
        return AYNI_STORAGE_FAILED;
    }
    auto rc = bodies_.configure(host_->value_store());
    if (rc != AYNI_OK) {
        status(0, "stage: bodies");
        return rc;
    }
    const uint32_t input_bytes = std::max({model_input_bytes(destination_), screen_input_bytes(),
                                           Speaker::input_bytes(), motor_input_bytes_max});
    const uint32_t result_bytes = std::max({static_cast<uint32_t>(ModelTools::wire_bytes),
                                            static_cast<uint32_t>(summary_bytes),
                                            Speaker::result_bytes, motor_reply_bytes_max});
    auto c = chat_config(input_bytes, result_bytes);
    size_t bytes = 0, alignment = 0;
    rc = ayni_kernel_memory_requirements(&c, &bytes, &alignment);
    if (rc != AYNI_OK) {
        return rc;
    }
    arena_ = heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    // The ToolLoop and the conversation codec query their regions; one serves both.
    size_t region_bytes = 0, region_alignment = 1;
    for (const auto query : {ayni::strategies::ToolLoop::workspace_requirements,
                             ayni::strategies::Conversation::workspace_requirements}) {
        size_t size = 0, align = 0;
        rc = query(&size, &align);
        if (rc != AYNI_OK) {
            return rc;
        }
        region_bytes = std::max(region_bytes, size);
        region_alignment = std::max(region_alignment, align);
    }
    auto *region = heap_caps_aligned_alloc(region_alignment, region_bytes,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    workspace_ = {sizeof(workspace_), 0, region, region_bytes};
    auto ports = host_->table();
    ports.bodies = bodies_.store(); // Core releases the bodies it stops naming.
    rc = ayni_create(&c, &ports, arena_, bytes, &kernel_);
    if (rc != AYNI_OK) {
        status(0, "stage: create");
    }
    if (rc == AYNI_OK) {
        rc = configure();
        if (rc != AYNI_OK) {
            status(0, "stage: configure");
        }
    }
    if (rc == AYNI_OK) {
        rc = ayni_start(kernel_);
        if (rc != AYNI_OK) {
            status(0, "stage: start");
        }
    }
    char message[160];
    std::snprintf(message, sizeof(message),
                  "Ayni ABI %u.%u; arena=%zu; internal_free=%zu; psram_free=%zu", AYNI_ABI_MAJOR,
                  AYNI_ABI_MINOR, bytes,
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    status(0, message);
    return rc;
}
ayni_status_t Chat::configure() {
    auto check = [&](const char *stage, ayni_status_t code) {
        if (code != AYNI_OK) {
            char message[96];
            std::snprintf(message, sizeof(message), "registration %s=%ld", stage, long(code));
            status(0, message);
        }
        return code;
    };
    const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, principal, version, 0};
    auto rc = check("ayni_register_principal", ayni_register_principal(kernel_, &owner));
    const ayni_agent_desc_v1 agent{sizeof(agent), 0, 1};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_agent", ayni_register_agent(kernel_, &agent));
    }
    if (rc == AYNI_OK) {
        rc = configure_model();
    }
    if (rc == AYNI_OK) {
        rc = configure_strategy();
    }
    if (rc == AYNI_OK) {
        rc = check("screen", configure_screen());
    }
    if (rc == AYNI_OK) {
        rc = check("speaker", speaker_.configure(kernel_));
    }
    if (rc == AYNI_OK) {
        rc = check("motor", motor_.configure(kernel_));
    }
    if (rc == AYNI_OK) {
        transport_.attach_speaker(&speaker_);
    }
    return rc;
}
// Two model capabilities on one Provider and one slot: tool-calling turns for the ToolLoop and
// text-only summaries for its compaction. Both egress to this boot's fixed destination.
ayni_status_t Chat::configure_model() {
    auto check = [&](const char *stage, ayni_status_t code) {
        if (code != AYNI_OK) {
            char message[96];
            std::snprintf(message, sizeof(message), "model registration %s=%ld", stage, long(code));
            status(0, message);
        }
        return code;
    };
    auto domain = sized<ayni_authority_value_set_v1>();
    domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    domain.values.tokens = {&destination_, 1, 0};
    const ayni_authority_field_desc_v1 field{sizeof(field), AYNI_MODEL_DESTINATION_FIELD_ID,
                                             AYNI_AUTHORITY_EGRESS_DESTINATION, 0, domain};
    auto rc = AYNI_OK;
    for (const char *id : {model_capability, summary_capability}) {
        const ayni_capability_desc_v1 cap{sizeof(cap), version, text(id), 1, 0, &field};
        if (rc == AYNI_OK) {
            rc = check("capability", ayni_register_capability(kernel_, &cap));
        }
    }
    resource_ = {text("model.slot"), 1, 0};
    input_schema_[0].kind = AYNI_VALUE_OBJECT;
    input_schema_[0].subtree_end = input_schema_.size();
    auto &destination = input_schema_[1];
    destination.kind = AYNI_VALUE_IDENTIFIER;
    destination.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    destination.subtree_end = 2;
    destination.name = text("destination");
    destination.flags = AYNI_SCHEMA_REQUIRED;
    destination.choice_count = 1;
    destination.choices = &destination_;
    // The prompt arrives by reference; its bytes must still fit the USB request lane.
    auto &body = input_schema_[2];
    body.kind = AYNI_VALUE_TEXT;
    body.field_id = AYNI_MODEL_BODY_FIELD_ID;
    body.subtree_end = input_schema_.size();
    body.name = text("body");
    body.flags = AYNI_SCHEMA_REQUIRED | AYNI_SCHEMA_EXTERNAL;
    body.max_count = tx_bytes;
    projection_.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    projection_.path_count = 1;
    projection_.path[0] = AYNI_MODEL_DESTINATION_FIELD_ID;
    for (uint32_t at = 0; at < business_.size(); ++at) {
        auto &result = result_schema_[at];
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = at ? summary_bytes : answer_bytes;
        business_[at] = {{input_schema_.data(), uint32_t(input_schema_.size())},
                         {&result, 1},
                         1,
                         0,
                         &projection_};
    }
    const uint32_t input_bytes = model_input_bytes(destination_);
    for (uint32_t at = 0; at < capabilities_.size(); ++at) {
        const bool tools = at == 0;
        capabilities_[at] = {
            sizeof(ayni_call_capability_v1),
            version,
            text(tools ? model_capability : summary_capability),
            {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL,
             AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING, AYNI_CALL_NOT_IDEMPOTENT,
             AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, harness::request_timeout_ms,
             AYNI_CALL_INPUT_VALUE, tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
             input_bytes,
             tools ? static_cast<uint32_t>(ModelTools::wire_bytes)
                   : static_cast<uint32_t>(summary_bytes),
             1, &resource_, nullptr},
            &business_[at]};
        if (rc == AYNI_OK) {
            rc = check("call capability",
                       ayni_register_call_capability(kernel_, &capabilities_[at]));
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 destination_,
                 text("usb.endpoint"),
                 destination_,
                 this,
                 uint32_t(capabilities_.size()),
                 0,
                 capabilities_.data(),
                 submit_model,
                 cancel_model,
                 nullptr,
                 nullptr};
    if (rc == AYNI_OK) {
        rc = check("provider", ayni_register_provider(kernel_, &provider_));
    }
    // One unit per concurrent model request the bridge serves (ADR-0095).
    const ayni_resource_desc_v1 resource{sizeof(resource),
                                         harness::model_concurrency == 1 ? AYNI_RESOURCE_EXCLUSIVE
                                                                         : AYNI_RESOURCE_SHARED,
                                         text("model.slot"),
                                         harness::model_concurrency,
                                         0,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
    if (rc == AYNI_OK) {
        rc = check("resource", ayni_register_resource(kernel_, &resource));
    }
    if (rc == AYNI_OK) {
        rc = check("health",
                   ayni_set_resource_health(kernel_, resource.resource_id, AYNI_RESOURCE_ONLINE));
    }
    for (uint32_t at = 0; rc == AYNI_OK && at < capabilities_.size(); ++at) {
        const ayni_binding_v1 binding{sizeof(binding),
                                      version,
                                      text(at == 0 ? model_binding : summary_binding),
                                      capabilities_[at].capability_id,
                                      version,
                                      version,
                                      provider_.provider_id,
                                      provider_.endpoint_id,
                                      destination_,
                                      0,
                                      0,
                                      nullptr};
        rc = check("binding", ayni_register_binding(kernel_, &binding));
    }
    return rc;
}
ayni_status_t Chat::configure_strategy() {
    auto domain = sized<ayni_authority_value_set_v1>();
    domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    domain.values.tokens = {&destination_, 1, 0};
    destination_field_ = {sizeof(destination_field_), AYNI_MODEL_DESTINATION_FIELD_ID, domain};
    destination_scope_ = {sizeof(destination_scope_), 1, &destination_field_};
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = text(strategy_id);
    config.model.kind = config.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = text(model_binding);
    config.summary.binding_id = text(summary_binding);
    config.model.arguments = config.summary.arguments = destination_scope_;
    config.model.text = text(instructions);
    config.limits = {harness::context_window_tokens, harness::max_output_tokens,
                     harness::compact_target_tokens, harness::tool_result_max_tokens};
    config.bodies = bodies_.store();
    config.history = {&bodies_, ayni::runtime::FileBodies::ready, nullptr, nullptr};
    auto rc = loop_.configure(config, &workspace_);
    if (rc == AYNI_OK) {
        inner_ = loop_.describe();
        strategy_ = inner_;
        strategy_.ctx = this;
        strategy_.decide = decide;
        strategy_.ready = admit;
        rc = ayni_register_strategy(kernel_, &strategy_);
    }
    const ayni_tool_ref_v1 tools[] = {
        {text("screen.show_text"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.play_tone"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.speak_text"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.status"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.plan"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.run"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.stop"), version, AYNI_TOOL_ROLE_CAPABILITY}};
    auto type = sized<ayni_goal_type_v1>();
    type.version = version;
    type.goal_type_id = text(strategy_id);
    type.strategy_id = strategy_.strategy_id;
    type.ctx = &workspace_;
    type.validate = ayni::strategies::Conversation::validate_turn;
    type.export_requirements = requirements;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.tool_count = sizeof(tools) / sizeof(tools[0]);
    type.tools = tools;
    type.tool_authorization = AYNI_TOOLS_ON_DEMAND;
    type.context_mode = AYNI_GOAL_CONTEXT_NONE; // History is this experiment's own store.
    if (rc == AYNI_OK) {
        rc = ayni_register_goal_type(kernel_, &type);
    }
    if (rc != AYNI_OK) {
        char message[96];
        std::snprintf(message, sizeof(message), "strategy registration=%ld", long(rc));
        status(0, message);
    }
    return rc;
}
ayni_status_t Chat::decide(void *ctx, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out) {
    auto &self = *static_cast<Chat *>(ctx);
    return self.inner_.decide(self.inner_.ctx, s, out);
}
// This interactive deployment starts a fresh session: until restored work is cancelled and
// readiness announced, and again once teardown began, no Task is admitted to decide.
uint32_t Chat::admit(void *ctx, uint64_t task) {
    auto &self = *static_cast<Chat *>(ctx);
    if (!self.announced_ || self.closing_ != Closing::open) {
        return 0;
    }
    return self.inner_.ready ? self.inner_.ready(self.inner_.ctx, task) : 1U;
}
ayni_status_t Chat::requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    for (const char *id : {model_capability, summary_capability}) {
        const ayni_goal_requirement_v1 req{sizeof(req), version, text(id), scope()};
        const auto rc = sink->emit(sink->ctx, &req);
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    return AYNI_OK;
}
// The prompt is read from the body store and projected to Chat Completions JSON for the Mac
// bridge, which adds the model name and key. Both local copies are wiped after submit; the
// request stays only in the USB transmit lane until the next frame overwrites it.
ayni_submit_result_t Chat::submit_model(void *ctx, const ayni_provider_request_v1 *request,
                                        ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Chat *>(ctx);
    if (!self.announced_ || self.closing_ != Closing::open || self.model_ != ModelState::idle ||
        !request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const bool summary = same(request->capability_id, summary_capability);
    if (!summary && !same(request->capability_id, model_capability)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    ayni_body_ref_v1 ref{};
    if (reader.read(request->value) != AYNI_OK ||
        !ayni::ValueReader::body(reader.field(AYNI_MODEL_BODY_FIELD_ID), ref) ||
        ref.size > self.request_body_.size()) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    struct Erase {
        std::array<char, tx_bytes> &prompt, &json;
        uint32_t size;
        size_t encoded = 0;
        ~Erase() {
            std::memset(prompt.data(), 0, size);
            std::memset(json.data(), 0, encoded);
        }
    } erase{self.request_body_, self.json_, ref.size};
    auto *bytes = reinterpret_cast<uint8_t *>(self.request_body_.data());
    const auto *store = self.bodies_.store();
    if (store->read(store->context, &ref, bytes, ref.size) != AYNI_OK ||
        ayni_body_verify(&ref, {bytes, ref.size}) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const ayni_string_v1 body{self.request_body_.data(), ref.size};
    const auto mode = summary ? ayni::model::Mode::text : ayni::model::Mode::assistant;
    ayni::json::Writer json(self.json_.data(), self.json_.size());
    json.raw("{\"max_tokens\":");
    json.unsigned_number(harness::max_output_tokens);
    json.raw(",");
    auto rc = AYNI_OK;
    constexpr auto prefix = ayni::model::messages_prefix;
    if (body.size >= prefix.size() && !std::memcmp(body.data, prefix.data(), prefix.size())) {
        rc = ayni::model::write_context(json, body, mode);
    } else {
        ayni_string_v1 system{}, data{};
        rc = ayni::model::split_instructions(body, system, data);
        json.raw("\"messages\":[");
        if (system.size) {
            json.raw("{\"role\":\"system\",\"content\":");
            json.string(system);
            json.raw("},");
        }
        json.raw("{\"role\":\"user\",\"content\":");
        json.string(data);
        json.raw("}]");
        if (!summary) {
            json.raw(",\"tools\":");
            ayni::model::write_functions(json);
        }
    }
    json.raw("}");
    size_t encoded_bytes = 0;
    const bool finished = json.finish(encoded_bytes);
    erase.encoded = encoded_bytes; // Bytes written, even after a capacity failure.
    if (rc != AYNI_OK || !finished ||
        !self.transport_.send(Kind::model_request, request->logical_call_id, self.json_.data(),
                              encoded_bytes)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.completion_ = completion;
    self.attempt_ = request->attempt;
    self.call_ = request->logical_call_id;
    self.deadline_ = request->deadline_us;
    self.summary_ = summary;
    self.model_ = ModelState::waiting;
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_submit_result_t Chat::cancel_model(void *ctx, ayni_attempt_ref_v1 attempt) {
    auto &self = *static_cast<Chat *>(ctx);
    if (self.model_ == ModelState::idle || attempt.boot_epoch != self.attempt_.boot_epoch ||
        attempt.request_id != self.attempt_.request_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (self.model_ == ModelState::waiting) {
        self.model_ = ModelState::cancelled;
        wake_owner(); // Inside ayni_step: the next tick posts the result without waiting.
    }
    return AYNI_SUBMIT_ACCEPTED;
}
void Chat::publish_model() {
    if (model_ == ModelState::waiting && uint64_t(esp_timer_get_time()) >= deadline_) {
        model_ = ModelState::failed;
    }
    if (model_ == ModelState::idle || model_ == ModelState::waiting) {
        return;
    }
    auto result = sized<ayni_provider_result_v1>();
    result.attempt = attempt_;
    if (model_ == ModelState::text || model_ == ModelState::tools) {
        result.outcome = AYNI_CALL_COMPLETED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        if (model_ == ModelState::tools) {
            result.tool_call_count = model_tools_.count();
            result.tool_calls = model_tools_.calls();
        } else {
            result.result = {model_text_.data(), uint32_t(model_text_size_)};
        }
    } else {
        // The HTTP request may already have billed/sent data. Cancellation cannot prove otherwise.
        result.outcome = AYNI_CALL_ABANDONED;
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    }
    if (ayni_provider_post(completion_, &result) == AYNI_OK) {
        model_ = ModelState::idle;
        completion_ = nullptr;
        call_ = deadline_ = 0;
        summary_ = false;
    }
}
ayni_status_t Chat::receipt() {
    if (!pending_command_) {
        return AYNI_OK;
    }
    auto r = sized<ayni_intent_receipt_v1>();
    const auto rc = ayni_get_intent_receipt(kernel_, principal, pending_command_, &r);
    if (rc == AYNI_OK && r.phase == AYNI_INTENT_DURABLE) {
        pending_command_ = 0;
        progress_ = true;
    }
    return rc;
}
ayni_status_t Chat::command(ayni_intent_kind_t kind, const void *body, uint64_t *id,
                            uint32_t revision) {
    auto rc = receipt();
    if (rc != AYNI_OK) {
        return rc;
    }
    if (pending_command_) {
        return AYNI_BUSY;
    }
    if (!boot_ || boot_ >= UINT64_MAX / command_range || sequence_ == command_range - 1) {
        return AYNI_INVALID_STATE;
    }
    const ayni_intent_v1 intent{
        sizeof(intent), kind, boot_ * command_range + ++sequence_, session_, revision, 0, body};
    auto out = sized<ayni_intent_result_v1>();
    auto error = sized<ayni_error_v1>();
    rc = ayni_submit_intent(kernel_, &intent, &out, &error);
    if (rc == AYNI_OK) {
        progress_ = true;
        if (out.phase != AYNI_INTENT_DURABLE) {
            pending_command_ = intent.command_id;
        }
        if (id) {
            *id = out.object.id;
        }
    }
    return rc;
}
void Chat::incoming(const Packet &p) {
    if (p.kind >= Kind::motor_decision && p.kind <= Kind::motor_heartbeat) {
        motor_.execution().receive(p.kind, p.id, p.body.data(), p.size);
        return;
    }
    if ((p.kind == Kind::model_text || p.kind == Kind::model_error ||
         p.kind == Kind::model_tools) &&
        model_ == ModelState::waiting && p.id == call_) {
        if (p.kind == Kind::model_tools) {
            // A summary asks for text only; proposals there are broken output.
            const auto rc =
                summary_ ? AYNI_INVALID_ARGUMENT : model_tools_.decode({p.body.data(), p.size});
            model_ = rc == AYNI_OK ? ModelState::tools : ModelState::failed;
            if (rc != AYNI_OK) {
                status(turn_, "model tool group invalid or over capacity; no tools dispatched");
            }
        } else if (p.kind == Kind::model_text && p.size &&
                   p.size <= (summary_ ? summary_bytes : answer_bytes)) {
            std::memcpy(model_text_.data(), p.body.data(), p.size);
            model_text_size_ = p.size;
            model_ = ModelState::text;
        } else {
            model_ = ModelState::failed;
        }
    } else if (p.kind == Kind::render_info && screen_ == ScreenState::waiting &&
               p.id == screen_call_ && p.size == sizeof(uint32_t)) {
        screen_pages_ = get32(p.body.data());
    } else if (p.kind == Kind::display_ack && screen_ == ScreenState::waiting &&
               p.id == screen_call_) {
        screen_ = p.size == 1 && p.body[0] == 1 ? ScreenState::painted : ScreenState::failed;
    } else if (p.kind == Kind::user) {
        if (!announced_ || turn_state_ != Turn::idle || forget_ != Forget::none ||
            closing_ != Closing::open || !history_.ready() || !p.size || p.size > input_bytes) {
            status(p.id, "input refused: not ready/busy/over limit");
            return;
        }
        // The text waits here until its body is durable and its Goal exists.
        input_.assign(reinterpret_cast<const char *>(p.body.data()), p.size);
        turn_ = p.id;
        hold_reported_ = false;
        turn_state_ = Turn::input;
        progress_ = true;
        status(turn_, "accepted");
    } else if (p.kind == Kind::cancel && turn_state_ == Turn::input) {
        end_turn("turn failed/cancelled before task creation; no automatic retry");
    } else if (p.kind == Kind::cancel && goal_) {
        // Owner command backpressure must not silently discard a user's cancellation.
        cancel_requested_ = true;
        progress_ = true;
        status(p.id, "cancel queued; waiting for current command receipt");
    } else if (p.kind == Kind::forget) {
        if (!announced_ || turn_state_ != Turn::idle || forget_ != Forget::none ||
            closing_ != Closing::open) {
            status(p.id, "context clear refused: device busy");
            return;
        }
        forget_ = Forget::requested;
        progress_ = true;
    } else if (p.kind == Kind::shutdown && closing_ == Closing::open) {
        closing_ = Closing::cancel_work;
        progress_ = true;
        speaker_.set_accepting(false);
        motor_.execution().set_accepting(false);
        motor_.execution().request_local_stop();
    }
}
// User-selected session policy: preserve history, cancel unfinished chat Goals,
// and wait for durable termination instead of replaying old screen/audio work.
ayni_status_t Chat::settle_session(uint64_t durable_seq, bool &settled) {
    settled = false;
    if (pending_command_) {
        return AYNI_OK;
    }
    uint64_t ids[goal_slots]{};
    uint32_t count = 0;
    auto rc = ayni_list_objects(kernel_, AYNI_OBJECT_GOAL, ids, goal_slots, &count);
    if (rc != AYNI_OK) {
        return rc;
    }
    for (uint32_t at = 0; at < count; ++at) {
        auto goal = sized<ayni_goal_info_v1>();
        rc = ayni_get_goal(kernel_, ids[at], &goal);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (goal.owner_instance_id != instance_ || goal.goal_type_version != version ||
            goal.goal_type_id_size != sizeof(strategy_id) - 1 ||
            std::memcmp(goal.goal_type_id, strategy_id, sizeof(strategy_id) - 1)) {
            continue;
        }
        if (goal.lifecycle == AYNI_GOAL_PROPOSED || goal.lifecycle == AYNI_GOAL_ACTIVE ||
            goal.lifecycle == AYNI_GOAL_PAUSED) {
            const ayni_goal_target_v1 target{sizeof(target), 0, goal.goal_id};
            rc = command(AYNI_INTENT_GOAL_CANCEL, &target, nullptr, goal.revision);
            if (rc == AYNI_OK) {
                char message[128];
                std::snprintf(message, sizeof(message),
                              "session cancel goal=%llu state=%lu wait=%lu; history preserved",
                              static_cast<unsigned long long>(goal.goal_id),
                              static_cast<unsigned long>(goal.lifecycle),
                              static_cast<unsigned long>(goal.wait_reason));
                status(0, message);
            }
            return rc == AYNI_BUSY ? AYNI_OK : rc;
        }
        if (goal.commit_seq > durable_seq) {
            return AYNI_OK;
        }
    }
    uint64_t tasks[task_slots]{};
    rc = ayni_list_objects(kernel_, AYNI_OBJECT_TASK, tasks, task_slots, &count);
    if (rc != AYNI_OK) {
        return rc;
    }
    for (uint32_t at = 0; at < count; ++at) {
        auto task = sized<ayni_task_info_v1>();
        rc = ayni_get_task(kernel_, tasks[at], &task);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (task.owner_instance_id == instance_ &&
            (task.commit_seq > durable_seq ||
             (task.lifecycle != AYNI_TASK_COMPLETED && task.lifecycle != AYNI_TASK_CANCELLED &&
              task.lifecycle != AYNI_TASK_FAILED && task.lifecycle != AYNI_TASK_EXPIRED))) {
            return AYNI_OK;
        }
    }
    settled = true;
    return AYNI_OK;
}
void Chat::diagnostics() {
    size_t used = 0;
    bool fits = true;
    auto append = [&](const char *format, auto... args) {
        if (!fits) {
            return;
        }
        const auto count = std::snprintf(json_.data() + used, json_.size() - used, format, args...);
        fits = count > 0 && size_t(count) < json_.size() - used;
        if (fits) {
            used += size_t(count);
        }
    };
    uint64_t ids[call_slots]{};
    uint32_t count = 0;
    append("session metadata (no message bodies): rounds=%llu",
           static_cast<unsigned long long>(history_.rounds()));
    if (ayni_list_objects(kernel_, AYNI_OBJECT_TASK, ids, call_slots, &count) == AYNI_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            auto task = sized<ayni_task_info_v1>();
            if (ayni_get_task(kernel_, ids[at], &task) == AYNI_OK) {
                append("\n task=%llu state=%lu hold=%ld/%lu", (unsigned long long)task.task_id,
                       (unsigned long)task.lifecycle, (long)task.hold_status,
                       (unsigned long)task.hold_reason);
            }
        }
    }
    if (ayni_list_objects(kernel_, AYNI_OBJECT_CALL, ids, call_slots, &count) == AYNI_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            auto call = sized<ayni_call_info_v1>();
            if (ayni_get_call(kernel_, ids[at], &call) != AYNI_OK) {
                continue;
            }
            append("\n call=%llu task=%llu phase=%lu outcome=%lu status=%ld reason=%lu effect=%lu "
                   "wait=%lu",
                   (unsigned long long)call.logical_call_id, (unsigned long long)call.task_id,
                   (unsigned long)call.phase, (unsigned long)call.outcome, (long)call.status,
                   (unsigned long)call.reason, (unsigned long)call.effect,
                   (unsigned long)call.wait_reason);
            auto execution = sized<ayni_execution_info_v1>();
            if (ayni_get_execution(kernel_, ids[at], &execution) == AYNI_OK) {
                append(" exec=%lu permit=%llu ended=%lu port=%ld", (unsigned long)execution.phase,
                       (unsigned long long)execution.permit_id,
                       (unsigned long)execution.execution_ended, (long)execution.executor_status);
            }
        }
    }
    if (fits) {
        transport_.send(Kind::status, 0, json_.data(), used);
    } else {
        status(0, "session metadata exceeded diagnostic capacity");
    }
}
ayni_status_t Chat::tick(ayni_step_report_v1 &report, bool &again) {
    again = progress_ = false;
    report = sized<ayni_step_report_v1>();
    report.next_deadline_us = UINT64_MAX;
    auto motor = motor_.execution().advance();
    if (motor != AYNI_OK && motor != AYNI_BUSY) {
        return motor;
    }
    publish_model();
    publish_screen();
    auto audio = speaker_.advance();
    if (audio != AYNI_OK && audio != AYNI_BUSY) {
        return audio;
    }
    auto info = sized<ayni_kernel_info_v1>();
    auto rc = ayni_get_kernel_info(kernel_, &info);
    // STOPPED cannot be stepped again while asynchronous device workers are joining.
    if (rc == AYNI_OK && info.lifecycle != AYNI_LIFECYCLE_STOPPED) {
        rc = ayni_step(kernel_, step_units, step_us, &report);
        if (rc == AYNI_OK) {
            rc = ayni_get_kernel_info(kernel_, &info);
        }
    }
    if (rc != AYNI_OK) {
        return rc;
    }
    if (info.lifecycle == AYNI_LIFECYCLE_STOPPED) {
        transport_.allow_display(0);
        if (!motor_.execution().close() || !speaker_.close()) {
            return AYNI_BUSY;
        }
        host_->close_storage();
        char speaker_metrics[96];
        std::snprintf(speaker_metrics, sizeof(speaker_metrics),
                      "metrics speaker_stack_free=%lu/12288",
                      static_cast<unsigned long>(speaker_.stack_free_bytes()));
        status(0, speaker_metrics);
        char metrics[180];
        std::snprintf(
            metrics, sizeof(metrics),
            "metrics owner_free=%lu/%lu worker_free=%lu/%lu internal_min=%zu psram_largest=%zu",
            static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
            static_cast<unsigned long>(owner_stack_capacity),
            static_cast<unsigned long>(host_->worker_stack_free_bytes()),
            static_cast<unsigned long>(host_->worker_stack_bytes),
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        status(0, metrics);
        rc = ayni_destroy(kernel_);
        if (rc == AYNI_OK) {
            kernel_ = nullptr;
            heap_caps_free(arena_);
            arena_ = nullptr;
            heap_caps_free(workspace_.data);
            workspace_ = {};
        }
        status(0, "stopped; reset board before reconnecting");
        return AYNI_NOT_READY;
    }
    if (info.lifecycle == AYNI_LIFECYCLE_FENCED || info.lifecycle == AYNI_LIFECYCLE_RESTRICTED) {
        // Core schema 8 media from the previous firmware open read-only (no migration).
        status(0, "Core storage restricted or fenced: state from an older firmware is read-only; "
                  "erase the ayni partition to start fresh");
        return AYNI_NOT_READY;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_OK;
    }
    rc = prepare(info);
    if (rc == AYNI_OK && announced_) {
        rc = converse(info);
    }
    again = progress_;
    return rc;
}
// Session, Grants, Scene and the conversation record, then the cleanup of restored work.
ayni_status_t Chat::prepare(const ayni_kernel_info_v1 &info) {
    if (!session_) {
        const ayni_session_desc_v1 desc{sizeof(desc), AYNI_CHANNEL_MANAGEMENT, principal, version,
                                        0};
        auto rc = ayni_open_session(kernel_, &desc, &session_);
        if (rc == AYNI_NOT_READY) {
            return AYNI_OK;
        }
        if (rc != AYNI_OK) {
            return rc;
        }
        auto agent = sized<ayni_agent_info_v1>();
        rc = ayni_get_agent(kernel_, 1, &agent);
        if (rc != AYNI_OK) {
            return rc;
        }
        instance_ = agent.instance_id;
        boot_ = info.boot_epoch;
        progress_ = true;
    }
    auto rc = receipt();
    if (rc != AYNI_OK) {
        return rc;
    }
    if (closing_ == Closing::cancel_work) {
        bool settled = false;
        rc = settle_session(info.durable_seq, settled);
        if (rc != AYNI_OK || !settled) {
            return rc;
        }
        rc = ayni_begin_shutdown(kernel_);
        if (rc == AYNI_OK) {
            closing_ = Closing::kernel;
            progress_ = true;
        }
        return rc;
    }
    if (grant_ < grant_count) {
        if (pending_command_) {
            return AYNI_OK;
        }
        // Reuse an exact, currently usable restored Grant instead of filling the
        // bounded table with a new permanent root after every ordinary reboot.
        uint64_t ids[grant_slots]{};
        uint32_t count = 0;
        rc = ayni_list_objects(kernel_, AYNI_OBJECT_GRANT, ids, grant_slots, &count);
        if (rc != AYNI_OK) {
            return rc;
        }
        const auto wanted = text(grant_capabilities[grant_]);
        for (uint32_t at = 0; at < count; ++at) {
            auto existing = sized<ayni_grant_info_v1>();
            rc = ayni_get_grant(kernel_, ids[at], &existing);
            if (rc != AYNI_OK) {
                return rc;
            }
            if (existing.subject_instance_id == instance_ && !existing.bound_goal_id &&
                existing.capability_version == version &&
                existing.effective_reason == AYNI_REASON_NONE &&
                existing.capability_id_size == wanted.size &&
                !std::memcmp(existing.capability_id, wanted.data, wanted.size)) {
                ++grant_;
                progress_ = true;
                return AYNI_OK;
            }
        }
        const bool motion = same(wanted, motion_capability);
        const ayni_grant_create_v1 grant{sizeof(grant),
                                         version,
                                         wanted,
                                         instance_,
                                         0,
                                         motion ? AYNI_GRANT_CURRENT_BOOT : AYNI_GRANT_PERMANENT,
                                         0,
                                         motion ? motor_grant_us : 0,
                                         scope()};
        const auto accepted = command(AYNI_INTENT_GRANT_CREATE, &grant);
        if (accepted == AYNI_OK) {
            ++grant_;
        }
        return accepted;
    }
    if (!announced_ && !pending_command_) {
        auto scene = sized<ayni_scene_info_v1>();
        rc = ayni_get_scene(kernel_, text(motor_scene), &scene);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (!scene.consumable) {
            const ayni_scene_set_enabled_v1 enabled{sizeof(enabled), 1, text(motor_scene), version,
                                                    0};
            return command(AYNI_INTENT_SCENE_SET_ENABLED, &enabled, nullptr, scene.revision);
        }
    }
    if (!history_configured_) {
        rc = history_.configure(&workspace_, &bodies_, host_->value_store(), instance_);
        if (rc != AYNI_OK) {
            return rc;
        }
        history_configured_ = true;
    }
    rc = history_.poll();
    if (rc != AYNI_OK) {
        char message[96];
        std::snprintf(message, sizeof(message), "conversation store failed=%ld; no answer shown",
                      long(rc));
        status(turn_, message);
        return rc;
    }
    if (!announced_ && !pending_command_ && history_.ready()) {
        bool settled = false;
        rc = settle_session(info.durable_seq, settled);
        if (rc != AYNI_OK || !settled) {
            return rc;
        }
        announced_ = true;
        progress_ = true; // Held Tasks may decide from the next step on.
        speaker_.set_accepting(true);
        motor_.execution().set_accepting(true);
        diagnostics();
        status(0, "ready");
    }
    return AYNI_OK;
}
void Chat::end_turn(const char *message) {
    if (message) {
        status(turn_, message);
    }
    history_.abandon(); // Drops the input body; the next turn starts from the saved record.
    input_.clear();
    goal_ = turn_ = 0;
    cancel_requested_ = false;
    turn_state_ = Turn::idle;
    progress_ = true;
}
// One user turn: durable input body, Goal, completed Task, durable record, then the answer.
ayni_status_t Chat::converse(const ayni_kernel_info_v1 &info) {
    if (forget_ == Forget::requested && turn_state_ == Turn::idle && history_.ready()) {
        const auto rc = history_.forget();
        if (rc == AYNI_OK) {
            forget_ = Forget::saving;
            progress_ = true;
        } else if (rc != AYNI_BUSY) {
            forget_ = Forget::none;
            status(0, "context clear failed; device context unchanged");
        }
    } else if (forget_ == Forget::saving && history_.ready()) {
        forget_ = Forget::none;
        status(0, "context cleared on device");
    }
    if (cancel_requested_ && goal_ && !pending_command_) {
        auto goal = sized<ayni_goal_info_v1>();
        auto rc = ayni_get_goal(kernel_, goal_, &goal);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (goal.lifecycle == AYNI_GOAL_ACTIVE || goal.lifecycle == AYNI_GOAL_PAUSED ||
            goal.lifecycle == AYNI_GOAL_PROPOSED) {
            const ayni_goal_target_v1 target{sizeof(target), 0, goal_};
            rc = command(AYNI_INTENT_GOAL_CANCEL, &target, nullptr, goal.revision);
            if (rc == AYNI_OK) {
                cancel_requested_ = false;
            }
            return rc == AYNI_BUSY ? AYNI_OK : rc;
        }
        cancel_requested_ = false;
    }
    if (turn_state_ == Turn::input && closing_ != Closing::open) {
        end_turn("turn failed/cancelled: shutting down before task creation; no automatic retry");
        return AYNI_OK;
    }
    if (turn_state_ == Turn::input) {
        const ayni_string_v1 user{input_.data(), static_cast<uint32_t>(input_.size())};
        auto rc = history_.begin(user, parameters_);
        if (rc == AYNI_NOT_READY || rc == AYNI_BUSY) {
            return AYNI_OK; // The input body is still being stored.
        }
        if (rc == AYNI_OK) {
            auto body = sized<ayni_goal_create_v1>();
            body.goal_type_version = version;
            body.goal_type_id = text(strategy_id);
            body.owner_instance_id = instance_;
            body.parameters = parameters_.view();
            body.trigger = AYNI_TRIGGER_ONCE;
            rc = command(AYNI_INTENT_GOAL_CREATE, &body, &goal_);
            if (rc == AYNI_BUSY) {
                return AYNI_OK;
            }
        }
        if (rc != AYNI_OK) {
            char error[64];
            std::snprintf(error, sizeof(error), "input error=%ld", long(rc));
            end_turn(error);
            return AYNI_OK;
        }
        input_.clear();
        turn_state_ = Turn::running;
        return AYNI_OK;
    }
    if (turn_state_ == Turn::running) {
        auto goal = sized<ayni_goal_info_v1>();
        auto rc = ayni_get_goal(kernel_, goal_, &goal);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (goal.wait_reason == AYNI_REASON_TASK_CAPACITY && !hold_reported_) {
            status(turn_, "task capacity full: retained unresolved history; input will be "
                          "cancelled, no retry");
            diagnostics();
            hold_reported_ = true;
            cancel_requested_ = true;
        }
        if (!goal.last_task_id) {
            if (goal.commit_seq <= info.durable_seq &&
                (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED)) {
                end_turn("turn failed/cancelled before task creation; no automatic retry");
            }
            return AYNI_OK;
        }
        auto task = sized<ayni_task_info_v1>();
        rc = ayni_get_task(kernel_, goal.last_task_id, &task);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (task.lifecycle == AYNI_TASK_COMPLETED && task.commit_seq <= info.durable_seq) {
            rc = ayni_copy_task_checkpoint(kernel_, task.task_id, parameters_.bytes.data(),
                                           static_cast<uint32_t>(parameters_.bytes.size()),
                                           &parameters_.size);
            if (rc == AYNI_OK) {
                rc = history_.finish(parameters_.view());
            }
            if (rc == AYNI_OK) {
                turn_state_ = Turn::saving;
                progress_ = true;
            } else if (rc != AYNI_BUSY) {
                char error[80];
                std::snprintf(error, sizeof(error),
                              "turn failed/cancelled: answer not saved=%ld; no automatic retry",
                              long(rc));
                end_turn(error);
            }
        } else if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_CANCELLED ||
                   task.lifecycle == AYNI_TASK_EXPIRED) {
            if (task.commit_seq <= info.durable_seq) {
                end_turn("turn failed/cancelled; no automatic retry");
            }
        } else if (task.lifecycle == AYNI_TASK_PAUSED && !hold_reported_) {
            char message[96];
            std::snprintf(message, sizeof(message), "task held status=%ld reason=%lu",
                          long(task.hold_status), static_cast<unsigned long>(task.hold_reason));
            status(turn_, message);
            hold_reported_ = true;
        }
        return AYNI_OK;
    }
    if (turn_state_ == Turn::saving) {
        if (!history_.take_answer(answer_)) {
            return AYNI_OK; // Shown only after the record naming it is durable.
        }
        turn_state_ = Turn::answer;
    }
    if (turn_state_ == Turn::answer) {
        if (answer_.size() > tx_bytes) {
            end_turn("turn failed/cancelled: answer exceeds the USB lane; not shown");
        } else if (transport_.send(Kind::answer, turn_, answer_.data(), answer_.size())) {
            answer_.clear();
            goal_ = turn_ = 0;
            cancel_requested_ = false;
            turn_state_ = Turn::idle;
            progress_ = true;
        }
    }
    return AYNI_OK;
}
} // namespace usbchat
