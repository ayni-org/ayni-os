#include "chat.h"
#include "configuration.h"
#include "motor_contract.h"
#include "synthetic_history.h"
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
namespace wifichat {
namespace {
constexpr uint64_t probe_record_id = UINT64_C(0x43545850524f4245);
constexpr uint32_t version = 1, step_units = 32, step_us = 4000;
constexpr uint64_t presentation_wait_us = UINT64_C(120) * 1000000;
constexpr uint64_t principal = 1;
constexpr char chat_type[] = "wifi.chat", probe_type[] = "wifi.context-probe";
constexpr char reply_capability[] = "wifi.model.text", reply_binding[] = "wifi.model",
               tools_capability[] = "wifi.model.tools", tools_binding[] = "wifi.tools";
constexpr const char *grant_capabilities[] = {reply_capability, tools_capability, "screen.text",
                                              "speaker.tone",   "speaker.speech", "motor.status",
                                              "motor.plan",     "motor.run",      "motor.stop"};
constexpr uint64_t motor_grant_us = UINT64_C(24) * 60 * 60 * 1000000;
constexpr uint64_t grant_count = sizeof(grant_capabilities) / sizeof(grant_capabilities[0]);
// Deployment-neutral identity; tool protocol and authorization rules come from
// the built-in ToolLoop, device details from the public capability directory.
constexpr char instructions[] =
    "你是Ayni，一个通用助手。根据用户的目标直接回答，或使用当前获准的能力完成请求。";
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
void Chat::trace(const char *phase) {
    if (turn_) {
        transport_.diagnostics().record(
            "wifi turn phase=%s model_requests=%lu elapsed_ms=%llu\n", phase,
            static_cast<unsigned long>(model_requests_),
            static_cast<unsigned long long>((uint64_t(esp_timer_get_time()) - turn_started_) /
                                            1000));
        trace_latency(phase);
    }
}
// Phase-local storage deltas plus cumulative owner work distinguish asynchronous waits from
// time spent running Core. These wall times overlap; they must not be summed as a turn total.
void Chat::trace_latency(const char *phase) {
    if (!host_) {
        return;
    }
    const auto current = host_->storage_metrics();
    const auto &last = timing_.storage;
    auto &log = transport_.diagnostics();
    log.record(
        "wifi latency phase=%s n=%llu queue_ms=%llu io_ms=%llu space_ms=%llu "
        "post_us=%llu idle_space_ms=%llu max_queue_ms=%llu max_io_ms=%llu max_space_ms=%llu\n",
        phase, current.completed - last.completed, (current.queue_us - last.queue_us) / 1000,
        (current.io_us - last.io_us) / 1000, (current.space_us - last.space_us) / 1000,
        current.post_us - last.post_us, (current.idle_space_us - last.idle_space_us) / 1000,
        current.max_queue_us / 1000, current.max_io_us / 1000, current.max_space_us / 1000);
    log.record("wifi io phase=%s append_ms=%llu value_ms=%llu scan_frames=%llu reads=%llu "
               "read_bytes=%llu read_ms=%llu write_ms=%llu sync_ms=%llu "
               "turn_core_ms=%llu steps=%llu max_step_us=%llu turn_converse_ms=%llu\n",
               phase, (current.append_us - last.append_us) / 1000,
               (current.value_us - last.value_us) / 1000, current.scan_frames - last.scan_frames,
               current.files.read_calls - last.files.read_calls,
               current.files.read_bytes - last.files.read_bytes,
               (current.files.read_us - last.files.read_us) / 1000,
               (current.files.write_us - last.files.write_us) / 1000,
               (current.files.sync_us - last.files.sync_us) / 1000, timing_.step_us / 1000,
               timing_.step_calls, timing_.max_step_us, timing_.conversation_us / 1000);
    timing_.storage = current;
}
void Chat::status(uint64_t id, const char *message) {
    if (!std::strncmp(message, "registration ", 13) ||
        !std::strncmp(message, "model registration ", 19) ||
        !std::strncmp(message, "strategy registration", 21) ||
        !std::strncmp(message, "stage:", 6) || !std::strncmp(message, "Ayni ABI ", 9)) {
        transport_.diagnostics().record("wifi init %s\n", message);
    }
    transport_.send(Kind::status, id, message, std::strlen(message));
}
ayni_status_t Chat::reserve_control_memory() {
    const auto motor = motor_.execution().reserve_control_stack();
    // Audio acquires I2S only while playing. Reserving its idle worker now keeps its
    // contiguous stack/buffers from competing with the later Host allocation.
    return speaker_.initialize() ? motor : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t Chat::initialize(const ModelSelection &selection) {
    const auto *route = selection.route.data();
    if (!route || std::strlen(route) != 16 || kernel_) {
        return AYNI_INVALID_ARGUMENT;
    }
    selection_ = selection;
    std::snprintf(route_, sizeof(route_), "model.%s", route);
    destination_ = text(route_);
    void *host_memory =
        heap_caps_malloc(sizeof(ayni::hosts::esp_idf::Host), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!host_memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    host_ = new (host_memory) ayni::hosts::esp_idf::Host;
    static_assert(host_capacity <= ayni::hosts::esp_idf::Host::request_capacity_limit);
    const auto opened = host_->open_storage("ayni", host_capacity, true);
    if (opened.status != ayni::hosts::esp_idf::StorageStatus::ok) {
        char error[96];
        std::snprintf(error, sizeof(error), "storage open=%u cause=%d", unsigned(opened.status),
                      opened.error);
        status(0, error);
        return AYNI_STORAGE_FAILED;
    }
    if (!values_.configure(host_->value_store())) {
        return AYNI_INVALID_STATE;
    }
    constexpr size_t cache_bytes = 2U * 1024U * 1024U;
    auto rc = bodies_.configure(values_.port(), cache_bytes);
    if (rc != AYNI_OK) {
        status(0, "stage: bodies");
        return rc;
    }
    bodies_.set_shared_call_results();
    transport_.attach_bodies(bodies_.store());
    const uint32_t input_bytes =
        std::max({model_input_bytes(destination_), screen_input_bytes(), Speaker::input_bytes()});
    const uint32_t result_bytes =
        std::max({static_cast<uint32_t>(summary_bytes), static_cast<uint32_t>(summary_bytes),
                  Speaker::result_bytes});
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
    transport_.diagnostics().record(
        "wifi init arena=%zu workspace=%zu internal_free=%zu psram_free=%zu\n", bytes, region_bytes,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
        rc = check("motor check", configure_motor_check());
    }
    if (rc == AYNI_OK) {
        auto speech = reply_voice_.strategy(bodies_.store());
        rc = check("reply voice strategy", ayni_register_strategy(kernel_, &speech));
        if (rc == AYNI_OK) {
            const auto type = ReplyVoice::goal_type();
            rc = check("reply voice type", ayni_register_goal_type(kernel_, &type));
        }
    }
    if (rc == AYNI_OK) {
        const auto prepared = motor_.execution().initialize();
        status(0,
               prepared == AYNI_OK ? "motor resources initialized" : "motor initialization failed");
        // A device failure is explicit, but does not prevent software-only conversation.
    }
    if (rc == AYNI_OK) {
        transport_.attach_speaker(&speaker_);
    }
    return rc;
}
ayni_status_t Chat::configure_motor_check() {
    MotorCheck::Owner owner;
    owner.context = this;
    owner.ready = [](const void *context) {
        const auto &self = *static_cast<const Chat *>(context);
        return self.announced_ && self.closing_ == Closing::open &&
               self.turn_state_ == Turn::idle && self.forget_ == Forget::none;
    };
    owner.submit = [](void *context, ayni_intent_kind_t kind, const void *body, uint64_t *id,
                      uint32_t revision) {
        MotorCheck::SubmitResult result;
        result.status =
            static_cast<Chat *>(context)->command(kind, body, id, revision, &result.reason);
        return result;
    };
    owner.stop = [](void *context) {
        static_cast<Chat *>(context)->transport_.request_motor_stop();
    };
    owner.notify = [](void *context, uint64_t id, const MotorCheckNotice &notice) {
        auto &self = *static_cast<Chat *>(context);
        std::array<uint8_t, MotorCheckNotice::wire_bytes> bytes{};
        notice.encode(bytes.data());
        const bool sent =
            self.transport_.send(Kind::motor_check_state, id, bytes.data(), bytes.size());
        if (sent) {
            self.transport_.diagnostics().record(
                "motor check request=%llu motor=%lu phase=%lu status=%ld reason=%lu\n",
                static_cast<unsigned long long>(id), static_cast<unsigned long>(notice.motor + 1),
                static_cast<unsigned long>(notice.phase), long(notice.status),
                static_cast<unsigned long>(notice.reason));
        }
        return sent;
    };
    auto status = motor_check_.configure(owner);
    return status == AYNI_OK ? motor_check_.install(kernel_) : status;
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
    for (const char *id : {reply_capability, tools_capability}) {
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
    auto &body = input_schema_[2];
    body.kind = AYNI_VALUE_TEXT;
    body.field_id = AYNI_MODEL_BODY_FIELD_ID;
    body.subtree_end = input_schema_.size();
    body.name = text("body");
    body.flags = AYNI_SCHEMA_REQUIRED | AYNI_SCHEMA_EXTERNAL;
    body.max_count = harness::model_body_bytes;
    projection_.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    projection_.path_count = 1;
    projection_.path[0] = AYNI_MODEL_DESTINATION_FIELD_ID;
    result_schema_.kind = AYNI_VALUE_TEXT;
    result_schema_.subtree_end = 1;
    result_schema_.max_count = summary_bytes;
    business_ = {{input_schema_.data(), uint32_t(input_schema_.size())},
                 {&result_schema_, 1},
                 1,
                 0,
                 &projection_};
    const uint32_t input_bytes = model_input_bytes(destination_);
    for (uint32_t at = 0; at < capabilities_.size(); ++at) {
        const bool tools = at == 1;
        capabilities_[at] = {
            sizeof(ayni_call_capability_v1),
            version,
            text(tools ? tools_capability : reply_capability),
            {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL,
             AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING, AYNI_CALL_NOT_IDEMPOTENT,
             AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, harness::request_timeout_ms,
             AYNI_CALL_INPUT_VALUE, tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT,
             input_bytes, static_cast<uint32_t>(summary_bytes), 1, &resource_, nullptr},
            &business_};
        if (rc == AYNI_OK) {
            rc = check("call capability",
                       ayni_register_call_capability(kernel_, &capabilities_[at]));
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 destination_,
                 text("wifi.endpoint"),
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
    // One unit per concurrent model request the API worker serves (ADR-0095).
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
                                      text(at == 0 ? reply_binding : tools_binding),
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
    config.identity.id = text(chat_type);
    config.model.kind = config.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = text(tools_binding);
    config.summary.binding_id = text(reply_binding);
    config.model.arguments = config.summary.arguments = destination_scope_;
    config.model.text = text(instructions);
    config.limits = {harness::context_window_tokens, harness::max_output_tokens,
                     harness::compact_target_tokens, harness::tool_result_max_tokens};
    config.bodies = bodies_.store();
    config.persistence = ayni::strategies::Persistence::background;
    config.body_progress = {&bodies_, ayni::runtime::FileBodies::progress};
    config.tool_exposure = ayni::strategies::ToolExposure::preloaded;
    config.reply_encoding = config.summary_reply_encoding = ayni::model::ReplyEncoding::framed;
    protocol_options_.model = selection_.model.data();
    protocol_options_.continuation_scope = selection_.scope.data();
    protocol_options_.api = selection_.api;
    protocol_options_.chat_token_limit = selection_.chat_token_limit;
    protocol_options_.reply_encoding = ayni::model::ReplyEncoding::framed;
    config.projection = ayni::providers::model_protocol::Protocol::projection(protocol_options_);
    auto status = loop_.configure(config, &workspace_);
    if (status != AYNI_OK) {
        return status;
    }
    inner_ = loop_.describe();
    strategy_ = inner_;
    strategy_.ctx = this;
    strategy_.decide = decide;
    strategy_.ready = admit;
    status = ayni_register_strategy(kernel_, &strategy_);
    if (status != AYNI_OK) {
        return status;
    }
    const ayni_tool_ref_v1 tools[] = {
        {text("screen.show_text"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.play_tone"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.speak_text"), SpeechProfile::capability_version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.status"), motor_version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.plan"), motor_version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.run"), motor_version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.stop"), motor_version, AYNI_TOOL_ROLE_CAPABILITY}};
    auto type = sized<ayni_goal_type_v1>();
    type.version = version;
    type.goal_type_id = text(probe_type);
    type.strategy_id = strategy_.strategy_id;
    type.ctx = &workspace_;
    type.validate = ayni::strategies::Conversation::validate_turn;
    type.export_requirements = requirements;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.tools = tools;
    type.tool_count = sizeof(tools) / sizeof(tools[0]);
    type.tool_authorization = AYNI_TOOLS_ON_DEMAND;
    // The local diagnostic does not attach to the real conversation context.
    type.context_mode = AYNI_GOAL_CONTEXT_NONE;
    status = ayni_register_goal_type(kernel_, &type);
    if (status == AYNI_OK) {
        type.goal_type_id = text(chat_type);
        type.context_mode = AYNI_GOAL_CONTEXT_AGENT;
        status = ayni_register_goal_type(kernel_, &type);
    }
    return status;
}
ayni_status_t Chat::decide(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                           ayni_policy_output_v1 *output) {
    auto &self = *static_cast<Chat *>(ctx);
    return self.inner_.decide(self.inner_.ctx, snapshot, output);
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
    for (const char *id : {tools_capability, reply_capability}) {
        const ayni_goal_requirement_v1 request{sizeof(request), version, text(id), scope()};
        const auto status = sink->emit(sink->ctx, &request);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
// The prompt is projected to Chat Completions JSON for the selected I/O backend,
// which adds the model name and key. Both owner-local copies are wiped after
// submit; bounded backend lanes retain their own copies for asynchronous I/O.
ayni_submit_result_t Chat::submit_model(void *ctx, const ayni_provider_request_v1 *request,
                                        ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Chat *>(ctx);
    if (!self.announced_ || self.closing_ != Closing::open || self.model_ != ModelState::idle ||
        !request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const bool summary = same(request->capability_id, reply_capability);
    if (!summary && !same(request->capability_id, tools_capability)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    ayni_body_ref_v1 ref{};
    if (reader.read(request->value) != AYNI_OK ||
        !ayni::ValueReader::body(reader.field(AYNI_MODEL_BODY_FIELD_ID), ref) || !ref.id ||
        !ref.size || ref.size > harness::model_body_bytes) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    // The Call retains this immutable body while the Provider is pending;
    // the HTTP worker reads it through the boot-lifetime body-store port.
    std::array<uint8_t, model_request_bytes> encoded{};
    put_body_reference(encoded.data(), ref);
    put32(encoded.data() + body_reference_bytes, summary ? 0 : 1);
    const auto kind = self.probe_.has_value() ? Kind::template_model_request : Kind::model_request;
    if (!self.transport_.send(kind, request->logical_call_id, encoded.data(), encoded.size())) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.completion_ = completion;
    self.attempt_ = request->attempt;
    self.call_ = request->logical_call_id;
    self.deadline_ = request->deadline_us;
    self.summary_ = summary;
    self.model_failure_ = {};
    self.model_ = ModelState::waiting;
    ++self.model_requests_;
    self.trace("model_requested");
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
        const auto *store = bodies_.store();
        const auto stored =
            model_body_.id ? AYNI_OK
                           : store->stage(store->context, call_,
                                          {reinterpret_cast<const uint8_t *>(model_answer_.data()),
                                           static_cast<uint32_t>(model_answer_.size())},
                                          &model_body_);
        if (stored == AYNI_NOT_READY) {
            return;
        }
        if (stored != AYNI_OK) {
            model_failure_ = {};
            model_failure_.struct_size = sizeof(model_failure_);
            model_failure_.outcome =
                stored == AYNI_RESULT_UNKNOWN ? AYNI_CALL_ABANDONED : AYNI_CALL_FAILED;
            model_failure_.status = stored;
            model_failure_.effect = AYNI_CALL_EFFECT_UNKNOWN;
            model_ = ModelState::failed;
            return;
        }
        result.body = model_body_;
        result.tool_call_count = model_tool_count_;
        result.model_usage = model_usage_;
    } else {
        if (model_body_.id) {
            bodies_.store()->release(bodies_.store()->context, &model_body_);
            model_body_ = {}; // Failed/cancelled delivery never transferred this output.
        }
        if (model_failure_.struct_size) {
            result.outcome = model_failure_.outcome;
            result.status = model_failure_.status;
            result.reason = model_failure_.reason;
            result.effect = model_failure_.effect;
        } else {
            // No complete classified terminal survived: disconnect/cancellation may have billed.
            result.outcome = AYNI_CALL_ABANDONED;
            result.status = AYNI_RESULT_UNKNOWN;
            result.effect = AYNI_CALL_EFFECT_UNKNOWN;
        }
    }
    if (ayni_provider_post(completion_, &result) == AYNI_OK) {
        model_body_ = {}; // The successful post transfers the reference to Core.
        trace("model_result_posted");
        model_ = ModelState::idle;
        completion_ = nullptr;
        call_ = deadline_ = 0;
        summary_ = false;
        std::fill(model_answer_.begin(), model_answer_.end(), 0);
        model_answer_.clear();
    }
}
ayni_status_t Chat::receipt() {
    const auto previous = commands_.pending();
    const auto status = commands_.poll();
    if (previous && !commands_.pending()) {
        progress_ = true;
    }
    return status;
}
ayni_status_t Chat::command(ayni_intent_kind_t kind, const void *body, uint64_t *id,
                            uint32_t revision, ayni_reason_t *reason) {
    const auto out = commands_.submit(kind, body, revision);
    if (reason) {
        *reason = out.error.reason;
    }
    if (out.status != AYNI_OK && out.status != AYNI_BUSY) {
        transport_.diagnostics().record(
            "wifi intent kind=%lu status=%ld reason=%lu object_kind=%lu object_id=%llu\n",
            static_cast<unsigned long>(kind), long(out.status),
            static_cast<unsigned long>(out.error.reason),
            static_cast<unsigned long>(out.error.object.kind),
            static_cast<unsigned long long>(out.error.object.id));
        diagnostics();
    }
    if (out.status == AYNI_OK) {
        progress_ = true;
        if (id) {
            *id = out.result.object.id;
        }
    }
    return out.status;
}
void Chat::incoming(const Packet &p) {
    if (p.kind == Kind::tool_outline) {
        if (p.id == call_ && p.size <= tool_outline_bytes &&
            (model_ == ModelState::text || model_ == ModelState::tools) &&
            reply_intent_ == ReplyIntent::present && closing_ == Closing::open) {
            transport_.send(Kind::tool_outline, turn_, p.body.data(), p.size);
        }
        return; // Metadata never changes model acceptance, execution or its terminal outcome.
    }
    if (p.kind == Kind::voice_reply_setting && p.size == 1 && p.body[0] <= 1) {
        voice_reply_enabled_ = p.body[0] != 0;
        voice_notice_ = VoiceNotice{p.id, p.body[0]};
        if (!voice_reply_enabled_) {
            reply_voice_.cancel();
        }
        return;
    }
    if (p.kind == Kind::motor_check) {
        auto code = AYNI_INVALID_ARGUMENT;
        if (p.size == sizeof(uint32_t)) {
            code = motor_check_.request(get32(p.body.data()), p.id);
        }
        if (code != AYNI_OK) {
            MotorCheckNotice failed{MotorCheckNotice::Phase::failed,
                                    p.size == sizeof(uint32_t) &&
                                            get32(p.body.data()) < MotorCheck::type_count
                                        ? get32(p.body.data())
                                        : 0,
                                    code, AYNI_REASON_NONE};
            std::array<uint8_t, MotorCheckNotice::wire_bytes> bytes{};
            failed.encode(bytes.data());
            transport_.send(Kind::motor_check_state, p.id, bytes.data(), bytes.size());
        }
        progress_ = true;
        return;
    }
    if (p.kind == Kind::cancel || p.kind == Kind::shutdown || p.kind == Kind::maintenance_stop) {
        motor_check_.cancel();
        reply_voice_.cancel();
    }
    if (p.kind == Kind::maintenance_stop) {
        // Keep file ports alive while the API worker exits its borrowed reads.
        announced_ = false;
        // A late UI request must not reopen shutdown, especially after destroy
        // while the reliable STOPPED report is still waiting for queue space.
        if (kernel_ && closing_ == Closing::open) {
            closing_ = Closing::quiescing;
        }
        stop_outputs();
        return;
    }
    if (p.kind == Kind::maintenance_shutdown) {
        // Rebuild and OTA both require confirmed STOPPED before touching their flash
        // regions. Public shutdown needs no spare command/Goal slot to cancel work.
        announced_ = false;
        stop_outputs();
        if (!kernel_ || closing_ == Closing::stopped || closing_ == Closing::kernel) {
            return; // Already closing/closed; keep the original STOPPED report authoritative.
        }
        // API borrows are drained. Complete the shared file/ACK bridge before Core closes;
        // no new Goal is created and accepted device work still follows public cancellation.
        closing_ = Closing::drain_session;
        progress_ = true;
        return;
    }
    if (p.kind == Kind::cancel && turn_state_ != Turn::idle) {
        reply_intent_ = ReplyIntent::cancelled;
    }
    if (p.kind == Kind::motor_settings || p.kind == Kind::motor_heartbeat ||
        p.kind == Kind::motor_power_off) {
        if (p.kind == Kind::motor_settings) {
            const auto status = motor_.execution().initialize();
            if (status != AYNI_OK) {
                char detail[96];
                std::snprintf(detail, sizeof(detail),
                              "motor preparation failed status=%ld; restart before retry",
                              long(status));
                this->status(0, detail);
                return;
            }
            transport_.set_motor_active(true);
        }
        motor_.execution().receive(p.kind, p.id, p.body.data(), p.size);
        return;
    }
    if ((p.kind == Kind::model_result || p.kind == Kind::model_error) &&
        model_ == ModelState::waiting && p.id == call_) {
        model_ = ModelState::failed;
        if (p.kind == Kind::model_error && p.size == model_result_header_bytes) {
            model_failure_ = read_model_result_header(p.body.data());
        }
        if (p.kind == Kind::model_result && p.size > model_result_header_bytes &&
            p.size <= tx_bytes) {
            const auto metadata = read_model_result_header(p.body.data());
            const auto count = metadata.tool_call_count;
            const ayni_bytes_v1 bytes{p.body.data() + model_result_header_bytes,
                                      p.size - static_cast<uint32_t>(model_result_header_bytes)};
            if (count <= AYNI_CALL_TOOL_COUNT_MAX && (!summary_ || !count)) {
                model_tool_count_ = count;
                model_usage_ = metadata.model_usage;
                model_answer_.assign(reinterpret_cast<const char *>(bytes.data), bytes.size);
                model_ = count ? ModelState::tools : ModelState::text;
            }
        }
    } else if (p.kind == Kind::render_info && screen_ == ScreenState::waiting &&
               p.id == screen_call_ && p.size == sizeof(uint32_t)) {
        screen_pages_ = get32(p.body.data());
    } else if (p.kind == Kind::display_ack && screen_ == ScreenState::waiting &&
               p.id == screen_call_) {
        screen_ = p.size == 1 && p.body[0] == 1 ? ScreenState::painted : ScreenState::failed;
    } else if (p.kind == Kind::user || p.kind == Kind::context_template) {
        const bool probe = p.kind == Kind::context_template;
        if (!announced_ || turn_state_ != Turn::idle || motor_check_.active() ||
            forget_ != Forget::none || closing_ != Closing::open || !history_.ready() ||
            (!probe && (!p.size || p.size > input_bytes))) {
            status(p.id, "input refused: not ready/busy/over limit");
            return;
        }
        probe_.reset();
        if (probe) {
            if ((p.size != 0 && p.size != sizeof(uint32_t)) ||
                (p.size && get32(p.body.data()) != current_history_probe &&
                 get32(p.body.data()) > harness::model_body_bytes / SyntheticHistory::han.size())) {
                status(p.id, "input refused: invalid diagnostic size");
                return;
            }
            probe_.emplace();
            const auto mode = p.size ? get32(p.body.data()) : 0;
            if (mode == current_history_probe) {
                probe_->source = ProbeTurn::Source::current_history;
                const auto *current = history_.record();
                transport_.diagnostics().record(
                    "wifi probe current_history rounds=%lu history_bytes=%lu no_model_http=1\n",
                    static_cast<unsigned long>(current ? current->round_count : 0),
                    static_cast<unsigned long>(current ? current->history_bytes : 0));
            } else {
                probe_->characters = mode;
            }
            transport_.diagnostics().record("wifi probe template_begin history_rounds=%llu\n",
                                            static_cast<unsigned long long>(rounds()));
            input_ = "synthetic context diagnostic";
        } else {
            input_.assign(reinterpret_cast<const char *>(p.body.data()), p.size);
        }
        turn_ = p.id;
        turn_started_ = uint64_t(esp_timer_get_time());
        timing_ = {};
        timing_.storage = host_->storage_metrics();
        model_requests_ = 0;
        reply_intent_ = ReplyIntent::present;
        hold_reported_ = false;
        preparation_wait_ = {};
        turn_state_ = Turn::input;
        progress_ = true;
        status(turn_, "accepted");
        trace("accepted");
    } else if (p.kind == Kind::cancel && turn_state_ == Turn::answer) {
        reply_end_ = "turn failed/cancelled; answer retained; no automatic retry";
        turn_state_ = Turn::reply_done;
        progress_ = true;
    } else if (p.kind == Kind::cancel && turn_state_ == Turn::input) {
        end_turn("turn failed/cancelled before task creation; no automatic retry");
    } else if (p.kind == Kind::cancel && (goal_ || history_.active())) {
        cancel_requested_ = true;
        progress_ = true;
        status(p.id, "cancel queued; waiting for current command receipt");
    } else if (p.kind == Kind::forget) {
        if (!announced_ || turn_state_ != Turn::idle || motor_check_.active() ||
            forget_ != Forget::none || closing_ != Closing::open) {
            status(p.id, "context clear refused: device busy");
            return;
        }
        forget_ = Forget::requested;
        progress_ = true;
    } else if (p.kind == Kind::shutdown && closing_ == Closing::open) {
        closing_ = Closing::cancel_work;
        progress_ = true;
        speaker_.set_accepting(false);
        speaker_.request_stop();
        motor_.execution().set_accepting(false);
        motor_.execution().request_stop();
    }
}
// User-selected session policy: preserve history, cancel unfinished chat Goals,
// and wait for durable termination instead of replaying old screen/audio work.
ayni_status_t Chat::settle_session(uint64_t durable_seq, bool &settled) {
    settled = false;
    if (commands_.pending()) {
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
        const bool chat_goal = goal.goal_type_version == version &&
                               (same({goal.goal_type_id, goal.goal_type_id_size}, chat_type) ||
                                same({goal.goal_type_id, goal.goal_type_id_size}, probe_type));
        const bool voice_goal =
            goal.goal_type_version == ReplyVoice::version &&
            same({goal.goal_type_id, goal.goal_type_id_size}, ReplyVoice::type_id);
        const bool check_goal =
            MotorCheck::owns({goal.goal_type_id, goal.goal_type_id_size}, goal.goal_type_version);
        if (goal.owner_instance_id != instance_) {
            continue;
        }
        if (!chat_goal && !check_goal && !voice_goal) {
            // Missing types remain held by Core. Reject this deployment's old data
            // explicitly instead of waiting forever for its held Tasks (ADR-0108).
            status(0, "unsupported retained goal type; data preserved");
            return AYNI_UNSUPPORTED_VERSION;
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
        const auto count =
            std::snprintf(diagnostics_.data() + used, diagnostics_.size() - used, format, args...);
        fits = count > 0 && size_t(count) < diagnostics_.size() - used;
        if (fits) {
            used += size_t(count);
        }
    };
    uint64_t ids[call_slots]{};
    uint32_t count = 0;
    const auto goal_status = ayni_list_objects(kernel_, AYNI_OBJECT_GOAL, ids, call_slots, &count);
    transport_.diagnostics().record(
        "wifi capacity goal_status=%ld goals=%lu/%lu tasks_max=%lu calls_max=%lu\n",
        long(goal_status), static_cast<unsigned long>(count),
        static_cast<unsigned long>(goal_slots), static_cast<unsigned long>(task_slots),
        static_cast<unsigned long>(call_slots));
    if (goal_status == AYNI_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            auto goal = sized<ayni_goal_info_v1>();
            if (ayni_get_goal(kernel_, ids[at], &goal) == AYNI_OK) {
                transport_.diagnostics().record("wifi goal id=%llu state=%lu task=%llu wait=%lu\n",
                                                static_cast<unsigned long long>(goal.goal_id),
                                                static_cast<unsigned long>(goal.lifecycle),
                                                static_cast<unsigned long long>(goal.last_task_id),
                                                static_cast<unsigned long>(goal.wait_reason));
            }
        }
    }
    append("session metadata (no message bodies): rounds=%llu",
           static_cast<unsigned long long>(rounds()));
    if (ayni_list_objects(kernel_, AYNI_OBJECT_TASK, ids, call_slots, &count) == AYNI_OK) {
        for (uint32_t at = 0; at < count; ++at) {
            auto task = sized<ayni_task_info_v1>();
            if (ayni_get_task(kernel_, ids[at], &task) == AYNI_OK) {
                transport_.diagnostics().record("wifi task id=%llu state=%lu hold=%ld/%lu\n",
                                                static_cast<unsigned long long>(task.task_id),
                                                static_cast<unsigned long>(task.lifecycle),
                                                long(task.hold_status),
                                                static_cast<unsigned long>(task.hold_reason));
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
            transport_.diagnostics().record(
                "wifi call id=%llu task=%llu phase=%lu outcome=%lu status=%ld effect=%lu\n",
                static_cast<unsigned long long>(call.logical_call_id),
                static_cast<unsigned long long>(call.task_id),
                static_cast<unsigned long>(call.phase), static_cast<unsigned long>(call.outcome),
                long(call.status), static_cast<unsigned long>(call.effect));
            append("\n call=%llu task=%llu phase=%lu outcome=%lu status=%ld reason=%lu effect=%lu "
                   "wait=%lu",
                   (unsigned long long)call.logical_call_id, (unsigned long long)call.task_id,
                   (unsigned long)call.phase, (unsigned long)call.outcome, (long)call.status,
                   (unsigned long)call.reason, (unsigned long)call.effect,
                   (unsigned long)call.wait_reason);
        }
    }
    if (fits) {
        transport_.send(Kind::status, 0, diagnostics_.data(), used);
    } else {
        status(0, "session metadata exceeded diagnostic capacity");
    }
}
ayni_status_t Chat::tick(ayni_step_report_v1 &report, bool &again) {
    again = progress_ = false;
    report = sized<ayni_step_report_v1>();
    report.next_deadline_us = UINT64_MAX;
    if (closing_ == Closing::stopped) {
        constexpr char stopped[] = "stopped; reset board before reconnecting";
        return transport_.send(Kind::status, 0, stopped, sizeof(stopped) - 1) ? AYNI_NOT_READY
                                                                              : AYNI_BUSY;
    }
    const auto motor = motor_.execution().advance();
    if (motor != AYNI_OK && motor != AYNI_BUSY) {
        return motor;
    }
    publish_model();
    publish_screen();
    auto audio = speaker_.advance();
    if (voice_notice_ && transport_.send(Kind::voice_reply_state, voice_notice_->id,
                                         &voice_notice_->enabled, sizeof(voice_notice_->enabled))) {
        voice_notice_.reset();
    }
    if (audio != AYNI_OK && audio != AYNI_BUSY) {
        return audio;
    }
    if (turn_state_ == Turn::reply_done) {
        // Drain the retained terminal before shutdown can release the shared session's file port.
        // A full UI queue defers this tick, rather than losing the only end-of-turn notice.
        const auto terminal = present_reply();
        if (terminal != AYNI_OK || turn_state_ == Turn::reply_done) {
            return terminal;
        }
    }
    auto info = sized<ayni_kernel_info_v1>();
    auto rc = ayni_get_kernel_info(kernel_, &info);
    // STOPPED cannot be stepped again while asynchronous device workers are joining.
    if (rc == AYNI_OK && info.lifecycle != AYNI_LIFECYCLE_STOPPED) {
        const auto started = uint64_t(esp_timer_get_time());
        rc = ayni_step(kernel_, step_units, step_us, &report);
        if (turn_) {
            const auto elapsed = uint64_t(esp_timer_get_time()) - started;
            ++timing_.step_calls;
            timing_.step_us += elapsed;
            timing_.max_step_us = std::max(timing_.max_step_us, elapsed);
        }
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
            closing_ = Closing::stopped;
            return AYNI_BUSY; // Give I/O time to drain metrics before the reliable terminal report.
        }
        return rc; // An unconfirmed destroy must not be reported as a completed shutdown.
    }
    if (info.lifecycle == AYNI_LIFECYCLE_FENCED || info.lifecycle == AYNI_LIFECYCLE_RESTRICTED) {
        // Many storage failures share these states. Only LOG_UNSUPPORTED means
        // incompatible media; preserve the public reason instead of suggesting erase.
        char detail[180];
        std::snprintf(detail, sizeof(detail),
                      "Core storage restricted/fenced: lifecycle=%lu restriction=%lu "
                      "boot=%llu durable=%llu; data preserved",
                      static_cast<unsigned long>(info.lifecycle),
                      static_cast<unsigned long>(info.restriction),
                      static_cast<unsigned long long>(info.boot_epoch),
                      static_cast<unsigned long long>(info.durable_seq));
        transport_.diagnostics().record("wifi storage %s\n", detail);
        status(0, detail);
        // The public shutdown contract also accepts FENCED. Keep stepping to
        // cancel Providers and drain borrowed I/O before closing the Host;
        // cached session IDs must not be reused after a storage reload.
        announced_ = false;
        speaker_.set_accepting(false);
        stop_outputs();
        rc = ayni_begin_shutdown(kernel_);
        if (rc == AYNI_OK) {
            closing_ = Closing::kernel;
            progress_ = again = true;
        }
        return rc;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_OK;
    }
    if (history_configured_) {
        auto privacy = sized<ayni_privacy_info_v1>();
        rc = ayni_get_privacy_status(kernel_, session_, &privacy);
        if (rc == AYNI_OK) {
            rc = loop_.maintain(kernel_, privacy);
        }
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    rc = prepare(info);
    if (rc == AYNI_OK) {
        motor_check_.tick(kernel_, instance_, info.durable_seq);
    }
    if (rc == AYNI_OK && announced_) {
        const auto started = uint64_t(esp_timer_get_time());
        rc = converse(info);
        if (turn_) {
            timing_.conversation_us += uint64_t(esp_timer_get_time()) - started;
        }
    }
    again = progress_;
    return rc;
}
// Session, Grants, Scene and the conversation record, then the cleanup of restored work.
// Bootstrap, deployment grants, current Scene and the shared session are separate owners.
ayni_status_t Chat::prepare_owner(const ayni_kernel_info_v1 &info) {
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
        rc = commands_.configure(kernel_, principal, session_, info.boot_epoch);
        if (rc != AYNI_OK) {
            return rc;
        }
        progress_ = true;
    }
    return AYNI_OK;
}
ayni_status_t Chat::prepare_grant() {
    auto rc = AYNI_OK;
    if (commands_.pending()) {
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
    const auto wanted_version =
        same(wanted, "speaker.speech") ? SpeechProfile::capability_version
        : std::string_view(wanted.data, wanted.size).substr(0, 6) == "motor." ? motor_version
                                                                              : version;
    for (uint32_t at = 0; at < count; ++at) {
        auto existing = sized<ayni_grant_info_v1>();
        rc = ayni_get_grant(kernel_, ids[at], &existing);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (existing.subject_instance_id == instance_ && !existing.bound_goal_id &&
            existing.capability_version == wanted_version &&
            existing.effective_reason == AYNI_REASON_NONE &&
            existing.capability_id_size == wanted.size &&
            !std::memcmp(existing.capability_id, wanted.data, wanted.size)) {
            ++grant_;
            progress_ = true;
            return AYNI_OK;
        }
    }
    const bool motion = same(wanted, "motor.run");
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     wanted_version,
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
ayni_status_t Chat::close_session(const ayni_kernel_info_v1 &info) {
    auto rc = AYNI_OK;
    if (closing_ == Closing::cancel_work) {
        bool settled = false;
        rc = settle_session(info.durable_seq, settled);
        if (rc != AYNI_OK || !settled) {
            return rc;
        }
    }
    if (history_configured_) {
        rc = history_.quiesce();
        if (rc == AYNI_BUSY) {
            return AYNI_OK;
        }
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    rc = ayni_begin_shutdown(kernel_);
    if (rc == AYNI_OK) {
        closing_ = Closing::kernel;
        progress_ = true;
    }
    return rc;
}
ayni_status_t Chat::prepare_history(const ayni_kernel_info_v1 &info) {
    auto rc = AYNI_OK;
    if (!history_configured_) {
        bool settled = false;
        rc = settle_session(info.durable_seq, settled);
        if (rc != AYNI_OK || !settled) {
            return rc;
        }
        auto anchor = sized<ayni_context_info_v1>();
        rc = ayni_get_context(kernel_, session_, instance_, &anchor);
        if (rc == AYNI_NOT_FOUND) {
            auto opened = sized<ayni_context_command_v1>();
            opened.owner_instance_id = instance_;
            rc = command(AYNI_INTENT_CONTEXT_OPEN, &opened);
            return rc == AYNI_BUSY ? AYNI_OK : rc;
        }
        if (rc != AYNI_OK || anchor.commit_seq > info.durable_seq) {
            return rc;
        }
        ayni::runtime::conversation::Config config;
        config.kernel = kernel_;
        config.session = session_;
        config.owner_instance = instance_;
        config.goal_type = text(chat_type);
        config.workspace = &workspace_;
        config.files = values_.port();
        config.bodies = bodies_.collection();
        config.commands = {
            this, [](void *context, ayni_intent_kind_t kind, const void *body, uint32_t revision) {
                auto &self = *static_cast<Chat *>(context);
                auto out = self.commands_.submit(kind, body, revision);
                self.progress_ = self.progress_ || out.status == AYNI_OK;
                return out;
            }};
        config.unfinished = ayni::runtime::conversation::UnfinishedTurn::discard;
        config.persistence = ayni::strategies::Persistence::background;
        rc = history_.configure(config);
        if (rc != AYNI_OK) {
            return rc;
        }
        history_configured_ = true;
    }
    if (cancel_requested_ && !probe_ && history_.active()) {
        // Stop admission precedes a sticky store error; receipt progress stays in prepare_owner.
        const auto cancelled = history_.cancel();
        if (cancelled == AYNI_OK) {
            cancel_requested_ = false;
        } else if (cancelled != AYNI_BUSY) {
            return cancelled;
        }
    }
    rc = history_.poll();
    if (rc != AYNI_OK && rc != AYNI_BUSY && !history_.held()) {
        status(turn_, "conversation store failed; no answer shown");
        return rc;
    }
    if (!announced_ && !commands_.pending() && history_.ready()) {
        announced_ = true;
        progress_ = true; // Held Tasks may decide from the next step on.
        speaker_.set_accepting(true);
        reply_voice_.set_accepting(true);
        motor_.execution().set_accepting(true);
        diagnostics();
        status(0, "ready");
    }
    return AYNI_OK;
}
ayni_status_t Chat::prepare(const ayni_kernel_info_v1 &info) {
    if (closing_ == Closing::quiescing) {
        return AYNI_OK;
    }
    auto rc = prepare_owner(info);
    // RUNNING can precede the durable owner session; bootstrap must finish before Grants.
    if (rc != AYNI_OK || !session_) {
        return rc;
    }
    if (rc == AYNI_OK) {
        rc = receipt();
    }
    if (rc != AYNI_OK) {
        return rc;
    }
    if (closing_ == Closing::cancel_work || closing_ == Closing::drain_session) {
        return close_session(info);
    }
    if (grant_ < grant_count) {
        return prepare_grant();
    }
    if (closing_ == Closing::open) {
        auto scene = sized<ayni_scene_info_v1>();
        rc = ayni_get_scene(kernel_, text(motor_scene), &scene);
        if (rc != AYNI_OK) {
            return rc;
        }
        if (!scene.consumable) {
            const ayni_scene_set_enabled_v1 enabled{sizeof(enabled), 1, text(motor_scene),
                                                    motor_version, 0};
            return command(AYNI_INTENT_SCENE_SET_ENABLED, &enabled, nullptr, scene.revision);
        }
    }
    return prepare_history(info);
}
void Chat::wait_for(const char *phase, const char *label, ayni_status_t code) {
    const auto now = uint64_t(esp_timer_get_time());
    if (preparation_wait_.phase != phase) {
        preparation_wait_ = {phase, 0};
        status(turn_, label);
    }
    if (now >= preparation_wait_.next_trace_us) {
        transport_.diagnostics().record(
            "wifi preparation phase=%s status=%ld pending=%llu goal=%llu elapsed_ms=%llu\n", phase,
            long(code), static_cast<unsigned long long>(commands_.pending()),
            static_cast<unsigned long long>(goal_),
            static_cast<unsigned long long>((now - turn_started_) / 1000));
        constexpr uint64_t trace_period_us = 2000000;
        preparation_wait_.next_trace_us = now + trace_period_us;
    }
}
void Chat::end_turn(const char *message) {
    if (message) {
        // Errors often use a stack-local buffer. Retain a copy and reopen input
        // only after the same reliable terminal lane as successful replies accepts it.
        std::snprintf(terminal_text_.data(), terminal_text_.size(), "%s", message);
        transport_.diagnostics().record("wifi terminal %s\n", terminal_text_.data());
        reply_end_ = terminal_text_.data();
        turn_state_ = Turn::reply_done;
        progress_ = true;
        return;
    }
    if (probe_.has_value()) {
        bodies_.collect(probe_record_id, {}); // No diagnostic checkpoint becomes chat history.
    }
    probe_.reset();
    trace_latency("terminal");
    transport_.diagnostics().record(
        "wifi turn model_requests=%lu elapsed_ms=%llu\n",
        static_cast<unsigned long>(model_requests_),
        static_cast<unsigned long long>((uint64_t(esp_timer_get_time()) - turn_started_) / 1000));
    input_.clear();
    answer_.clear();
    reply_voice_.reset();
    reported_voice_failure_ = AYNI_OK;
    reply_deadline_ = 0;
    reply_end_ = nullptr;
    reply_intent_ = ReplyIntent::present;
    goal_ = turn_ = 0;
    cancel_requested_ = false;
    turn_state_ = Turn::idle;
    progress_ = true;
}
ayni_status_t Chat::probe_input(ayni_string_v1 user, ayni::strategies::Encoded &parameters) {
    auto &probe = *probe_;
    ayni::strategies::ConversationRecord record{};
    record.id = probe_record_id;
    record.owner_instance_id = instance_;
    if (probe.source == ProbeTurn::Source::current_history) {
        const auto *current = history_.record();
        if (!current) {
            return AYNI_NOT_READY;
        }
        record = *current;
        record.id = probe_record_id; // Private writes/cleanup cannot alter real history owners.
        record.draft = {};
    }
    if (probe.characters && !probe.history_ref.id) {
        if (!probe.history) {
            const auto bytes =
                ayni::model::messages_prefix.size() +
                SyntheticHistory::room(SyntheticHistory::Corpus::han, probe.characters);
            // The isolated test must refuse before forcing the SDK's STL working copies
            // into OOM. Reserve for cached history, policy copies and both request lanes;
            // this conservative test guard is not the application's context-window limit.
            constexpr size_t history_copies = 6, codec_headroom = 128 * 1024;
            constexpr size_t internal_headroom = 16 * 1024;
            const auto psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            const auto internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const auto required = history_copies * bytes + codec_headroom;
            if (required > psram || internal < internal_headroom) {
                transport_.diagnostics().record(
                    "wifi probe preflight characters=%lu required_psram=%llu available_psram=%zu "
                    "internal_free=%zu status=%ld\n",
                    static_cast<unsigned long>(probe.characters),
                    static_cast<unsigned long long>(required), psram, internal,
                    long(AYNI_CAPACITY_EXCEEDED));
                return AYNI_CAPACITY_EXCEEDED;
            }
            probe.history.reset(new (std::nothrow) wifi::ModelBuffer(bytes));
            if (!probe.history || !probe.history->data) {
                transport_.diagnostics().record(
                    "wifi probe history_memory characters=%lu status=%ld\n",
                    static_cast<unsigned long>(probe.characters), long(AYNI_CAPACITY_EXCEEDED));
                return AYNI_CAPACITY_EXCEEDED;
            }
            ayni::model::Writer out(reinterpret_cast<char *>(probe.history->data), uint32_t(bytes));
            out.prefix();
            SyntheticHistory::append(out, SyntheticHistory::Corpus::han, probe.characters);
            ayni_string_v1 history{};
            if (!out.finish(history)) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            probe.history_bytes = history.size;
            transport_.diagnostics().record(
                "wifi probe loop_begin characters=%lu history_bytes=%lu\n",
                static_cast<unsigned long>(probe.characters),
                static_cast<unsigned long>(history.size));
        }
        const auto *store = bodies_.store();
        const auto status =
            store->write(store->context, probe_record_id,
                         {probe.history->data, probe.history_bytes}, &probe.history_ref);
        if (status != AYNI_OK) {
            return status;
        }
        probe.history.reset(); // FileBodies owns its real cached copy, just as for user history.
    }
    if (probe.history_ref.id) {
        record.history = probe.history_ref;
        record.last_round = 1; // Synthetic prior messages; no real conversation record is changed.
    }
    return ayni::strategies::Conversation::begin(&workspace_, *bodies_.store(), record, user,
                                                 parameters);
}
// Isolated capacity diagnostics observe Core without publishing to the real conversation.
ayni_status_t Chat::observe_probe(const ayni_kernel_info_v1 &info) {
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
        transport_.diagnostics().record(
            "wifi probe loop_complete characters=%lu model_requests=%lu\n",
            static_cast<unsigned long>(probe_->characters),
            static_cast<unsigned long>(model_requests_));
        transport_.diagnostics().record("wifi probe template_complete history_rounds=%llu\n",
                                        static_cast<unsigned long long>(rounds()));
        goal_ = 0;
        reply_end_ = "reply completed";
        turn_state_ = Turn::reply_done;
        progress_ = true;
        return AYNI_OK; // Diagnostic model reply is deliberately not saved or shown.
    } else if (task.lifecycle == AYNI_TASK_FAILED || task.lifecycle == AYNI_TASK_CANCELLED ||
               task.lifecycle == AYNI_TASK_EXPIRED) {
        if (task.commit_seq <= info.durable_seq) {
            end_turn("turn failed/cancelled; no automatic retry");
        }
    } else if (task.lifecycle == AYNI_TASK_PAUSED && !hold_reported_) {
        char message[96];
        std::snprintf(message, sizeof(message), "task held status=%ld reason=%lu",
                      long(task.hold_status), static_cast<unsigned long>(task.hold_reason));
        transport_.diagnostics().record("wifi held status=%ld reason=%lu\n", long(task.hold_status),
                                        static_cast<unsigned long>(task.hold_reason));
        status(turn_, message);
        hold_reported_ = true;
        diagnostics(); // Capture the same held call before a later reset loses the trace.
    }
    return AYNI_OK;
}
// One user turn: durable input body, Goal, completed Task, durable record, then the answer.
ayni_status_t Chat::converse(const ayni_kernel_info_v1 &info) {
    if (forget_ == Forget::requested && turn_state_ == Turn::idle && history_.ready()) {
        const auto rc = history_.forget(AYNI_CONTEXT_ERASE_ALL);
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
    if (cancel_requested_ && probe_ && goal_ && !commands_.pending()) {
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
        const char *failure_stage = "input storage";
        auto rc = AYNI_OK;
        if (probe_.has_value()) {
            rc = probe_input(user, parameters_);
        } else {
            rc = history_.begin(user);
        }
        if (rc == AYNI_NOT_READY || rc == AYNI_BUSY) {
            wait_for("input_body", "preparing input: waiting for input storage", rc);
            return AYNI_OK; // The input body is still being stored.
        }
        if (rc == AYNI_OK && probe_) {
            failure_stage = "goal creation";
            auto body = sized<ayni_goal_create_v1>();
            body.goal_type_version = version;
            body.goal_type_id = text(probe_type);
            body.owner_instance_id = instance_;
            body.parameters = parameters_.view();
            body.trigger = AYNI_TRIGGER_ONCE;
            rc = command(AYNI_INTENT_GOAL_CREATE, &body, &goal_);
            if (rc == AYNI_OK || rc == AYNI_BUSY) {
                wait_for("goal_create", "preparing input: creating task", rc);
            }
            if (rc == AYNI_BUSY) {
                return AYNI_OK;
            }
        }
        if (rc != AYNI_OK) {
            char error[96];
            std::snprintf(error, sizeof(error), "turn failed: %s status=%ld; no automatic retry",
                          failure_stage, long(rc));
            end_turn(error);
            return AYNI_OK;
        }
        input_.clear();
        turn_state_ = Turn::running;
        return AYNI_OK;
    }
    if (turn_state_ == Turn::running && !probe_) {
        goal_ = history_.latest_goal();
        auto goal = sized<ayni_goal_info_v1>();
        if (goal_ && !hold_reported_) {
            const auto queried = ayni_get_goal(kernel_, goal_, &goal);
            if (queried != AYNI_OK) {
                return queried;
            }
            if (goal.wait_reason == AYNI_REASON_TASK_CAPACITY) {
                status(turn_, "task capacity full; cancelling input without retry");
                hold_reported_ = true;
                return history_.cancel();
            }
        }
        if (history_.held()) {
            if (!hold_reported_) {
                auto task = sized<ayni_task_info_v1>();
                const auto queried = ayni_get_task(kernel_, goal.last_task_id, &task);
                if (queried != AYNI_OK) {
                    return queried;
                }
                char detail[96];
                std::snprintf(detail, sizeof(detail), "task held status=%ld reason=%lu",
                              long(task.hold_status), static_cast<unsigned long>(task.hold_reason));
                status(turn_, detail);
                diagnostics();
                hold_reported_ = true;
            }
            return AYNI_OK;
        }
        ayni::runtime::conversation::TurnResult result;
        if (!history_.take_result(result)) {
            return AYNI_OK;
        }
        if (!result.published) {
            char detail[128];
            std::snprintf(detail, sizeof(detail),
                          "turn failed/cancelled status=%ld reason=%lu; no automatic retry",
                          long(result.status), static_cast<unsigned long>(result.reason));
            end_turn(detail);
            return AYNI_OK;
        }
        turn_state_ = Turn::saving;
    }
    if (turn_state_ == Turn::running && probe_) {
        return observe_probe(info);
    }
    if (turn_state_ == Turn::saving) {
        const auto *record = history_.record();
        const auto rc = record ? ayni::strategies::Conversation::latest_answer(*bodies_.store(),
                                                                               *record, answer_)
                               : AYNI_NOT_READY;
        if (rc == AYNI_NOT_READY) {
            return AYNI_OK;
        }
        if (rc != AYNI_OK) {
            end_turn("turn failed: saved answer unavailable");
            return AYNI_OK;
        }
        turn_state_ = Turn::answer;
        trace("answer_saved");
        reply_deadline_ = uint64_t(esp_timer_get_time()) + presentation_wait_us;
        goal_ = 0; // The completed task has no further device work.
    }
    if (turn_state_ == Turn::answer || turn_state_ == Turn::speaking ||
        turn_state_ == Turn::reply_done) {
        return present_reply();
    }
    return AYNI_OK;
}
// Publish the saved text first. Opt-in automatic speech then runs as separate authorized Goals;
// the original Task and its conversation publication are never replayed for presentation.
ayni_status_t Chat::present_reply() {
    if (turn_state_ == Turn::answer) {
        if (reply_intent_ == ReplyIntent::cancelled) {
            reply_end_ = "turn failed/cancelled; answer retained; no automatic retry";
            turn_state_ = Turn::reply_done;
        } else if (uint64_t(esp_timer_get_time()) >= reply_deadline_) {
            reply_end_ = "turn failed: answer delivery timed out; answer retained";
            turn_state_ = Turn::reply_done;
        } else if (answer_.empty() || answer_.size() > tx_bytes) {
            reply_end_ = "turn failed: answer exceeds chat capacity";
            turn_state_ = Turn::reply_done;
        } else if (transport_.send(Kind::answer, turn_, answer_.data(), answer_.size())) {
            reply_end_ = "reply completed";
            if (voice_reply_enabled_) {
                auto source = sized<ayni_context_info_v1>();
                const auto queried = ayni_get_context(kernel_, session_, instance_, &source);
                const auto started =
                    queried == AYNI_OK ? reply_voice_.begin(std::move(answer_), source) : queried;
                if (started == AYNI_OK) {
                    turn_state_ = Turn::speaking;
                    status(turn_, "automatic voice reply started");
                } else {
                    finish_voice(started);
                }
            } else {
                turn_state_ = Turn::reply_done;
            }
            progress_ = true;
        }
    }
    if (turn_state_ == Turn::speaking) {
        if (reply_intent_ == ReplyIntent::cancelled || closing_ != Closing::open) {
            reply_voice_.cancel();
        }
        auto info = sized<ayni_kernel_info_v1>();
        const auto queried = ayni_get_kernel_info(kernel_, &info);
        if (queried != AYNI_OK) {
            return queried;
        }
        const auto result =
            reply_voice_.advance(kernel_, commands_, session_, instance_, info.durable_seq);
        if (result == AYNI_BUSY && reply_voice_.failure_status() != reported_voice_failure_) {
            reported_voice_failure_ = reply_voice_.failure_status();
            status(turn_, "automatic voice reply draining after error");
            transport_.diagnostics().record("wifi voice draining status=%ld\n",
                                            long(reported_voice_failure_));
        }
        if (result != AYNI_BUSY) {
            finish_voice(result);
        }
    }
    if (turn_state_ == Turn::reply_done &&
        transport_.send(Kind::status, turn_, reply_end_, std::strlen(reply_end_))) {
        end_turn(nullptr); // Terminal UI status is accepted before reopening input.
    }
    return AYNI_OK;
}
void Chat::finish_voice(ayni_status_t result) {
    if (result != AYNI_OK) {
        std::snprintf(terminal_text_.data(), terminal_text_.size(),
                      "reply completed; voice failed status=%ld; no automatic retry", long(result));
        reply_end_ = terminal_text_.data();
    }
    turn_state_ = Turn::reply_done;
}
} // namespace wifichat
