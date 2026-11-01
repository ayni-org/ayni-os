#include "chat.h"
#include "configuration.h"
#include "motor_contract.h"
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
constexpr uint32_t version = 1, step_units = 32, step_us = 4000, model_timeout_ms = 90000;
constexpr uint64_t principal = 1, command_range = UINT64_C(1) << 32;
// Boot-local motion authority also expires after one day; no silent renewal.
constexpr uint32_t motor_run_grant_index = 6;
constexpr uint64_t motor_grant_us = UINT64_C(24) * 60 * 60 * 1000000;
constexpr char capability_id[] = "usb.model.text", binding_id[] = "usb.model",
               strategy_id[] = "usb.chat";
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
} // namespace
void Chat::status(uint64_t id, const char *message) {
    transport_.send(Kind::status, id, message, std::strlen(message));
}
ayni_status_t Chat::initialize(const char *route) {
    if (!route || std::strlen(route) != 16 || kernel_) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::snprintf(route_, sizeof(route_), "model.%s", route);
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
    auto c = chat_config();
    size_t bytes = 0, alignment = 0;
    auto rc = ayni_kernel_memory_requirements(&c, &bytes, &alignment);
    if (rc != AYNI_OK) {
        return rc;
    }
    arena_ = heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    size_t value_bytes = 0, value_alignment = 0;
    rc = ayni_value_workspace_requirements(&value_bytes, &value_alignment);
    if (rc != AYNI_OK) {
        return rc;
    }
    auto *value_memory =
        heap_caps_aligned_alloc(value_alignment, value_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!value_memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    value_workspace_ = {sizeof(value_workspace_), 0, value_memory, value_bytes};
    const auto ports = host_->table();
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
    destination_ = text(route_);
    auto domain = sized<ayni_authority_value_set_v1>();
    domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    domain.values.tokens = {&destination_, 1, 0};
    const ayni_authority_field_desc_v1 field{sizeof(field), AYNI_MODEL_DESTINATION_FIELD_ID,
                                             AYNI_AUTHORITY_EGRESS_DESTINATION, 0, domain};
    const ayni_capability_desc_v1 cap{sizeof(cap), version, text(capability_id), 1, 0, &field};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_capability", ayni_register_capability(kernel_, &cap));
    }
    resource_ = {text("model.slot"), 1, 0};
    input_[0].kind = AYNI_VALUE_OBJECT;
    input_[0].subtree_end = input_.size();
    auto &destination = input_[1];
    destination.kind = AYNI_VALUE_IDENTIFIER;
    destination.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    destination.subtree_end = 2;
    destination.name = text("destination");
    destination.flags = AYNI_SCHEMA_REQUIRED;
    destination.choice_count = 1;
    destination.choices = &destination_;
    auto &body = input_[2];
    body.kind = AYNI_VALUE_TEXT;
    body.field_id = AYNI_MODEL_BODY_FIELD_ID;
    body.subtree_end = input_.size();
    body.name = text("body");
    body.flags = AYNI_SCHEMA_REQUIRED;
    body.max_count = prompt_bytes;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = answer_bytes;
    projection_.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    projection_.path_count = 1;
    projection_.path[0] = AYNI_MODEL_DESTINATION_FIELD_ID;
    business_ = {{input_.data(), uint32_t(input_.size())}, {&result_, 1}, 1, 0, &projection_};
    capability_ = {sizeof(capability_),
                   version,
                   text(capability_id),
                   {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL,
                    AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING, AYNI_CALL_NOT_IDEMPOTENT,
                    AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, model_timeout_ms,
                    AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TOOL_CALLS, prompt_bytes + 256, 8192, 1,
                    &resource_, nullptr},
                   &business_};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_call_capability",
                   ayni_register_call_capability(kernel_, &capability_));
    }
    provider_ = {sizeof(provider_),
                 version,
                 destination_,
                 text("usb.endpoint"),
                 destination_,
                 this,
                 1,
                 0,
                 &capability_,
                 submit_model,
                 cancel_model,
                 nullptr,
                 nullptr};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_provider", ayni_register_provider(kernel_, &provider_));
    }
    const ayni_resource_desc_v1 resource{
        sizeof(resource), AYNI_RESOURCE_EXCLUSIVE, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_resource", ayni_register_resource(kernel_, &resource));
    }
    if (rc == AYNI_OK) {
        rc = check("ayni_set_resource_health",
                   ayni_set_resource_health(kernel_, resource.resource_id, AYNI_RESOURCE_ONLINE));
    }
    const ayni_binding_v1 binding{sizeof(binding),
                                  version,
                                  text(binding_id),
                                  text(capability_id),
                                  version,
                                  version,
                                  provider_.provider_id,
                                  provider_.endpoint_id,
                                  destination_,
                                  0,
                                  0,
                                  nullptr};
    if (rc == AYNI_OK) {
        rc = check("ayni_register_binding", ayni_register_binding(kernel_, &binding));
    }
    auto strategy = sized<ayni_strategy_v1>();
    strategy.version = version;
    strategy.strategy_id = text(strategy_id);
    strategy.ctx = this;
    strategy.decide = decide;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = prompt_bytes + 16;
    if (rc == AYNI_OK) {
        rc = check("ayni_register_strategy", ayni_register_strategy(kernel_, &strategy));
    }
    auto type = sized<ayni_goal_type_v1>();
    type.version = version;
    type.goal_type_id = text(strategy_id);
    type.strategy_id = text(strategy_id);
    type.validate = validate;
    type.export_requirements = requirements;
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    const ayni_tool_ref_v1 tools[] = {
        {text("screen.show_text"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.play_tone"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("speaker.speak_text"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.status"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.plan"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.run"), version, AYNI_TOOL_ROLE_CAPABILITY},
        {text("motor.stop"), version, AYNI_TOOL_ROLE_CAPABILITY}};
    type.tool_count = sizeof(tools) / sizeof(tools[0]);
    type.tools = tools;
    type.tool_authorization = AYNI_TOOLS_ON_DEMAND;
    if (rc == AYNI_OK) {
        rc = check("ayni_register_goal_type", ayni_register_goal_type(kernel_, &type));
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
ayni_status_t Chat::validate(void *, ayni_bytes_v1 bytes) {
    return bytes.data && bytes.size && bytes.size <= prompt_bytes ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}
ayni_status_t Chat::requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const ayni_goal_requirement_v1 req{sizeof(req), version, text(capability_id), scope()};
    return sink->emit(sink->ctx, &req);
}
ayni_submit_result_t Chat::submit_model(void *ctx, const ayni_provider_request_v1 *request,
                                        ayni_provider_completion_t *completion) {
    auto &self = *static_cast<Chat *>(ctx);
    if (!self.announced_ || self.closing_ != Closing::open || self.model_ != ModelState::idle ||
        !request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    ayni_string_v1 body{};
    if (reader.read(request->value) != AYNI_OK ||
        !ayni::ValueReader::text(reader.field(AYNI_MODEL_BODY_FIELD_ID), body) ||
        body.size > prompt_bytes) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::json::Writer json(self.json_.data(), self.json_.size());
    json.raw("{");
    if (ayni::model::write_context(json, body, ayni::model::Mode::assistant) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    json.raw("}");
    size_t encoded_bytes = 0;
    if (!json.finish(encoded_bytes) ||
        !self.transport_.send(Kind::model_request, request->logical_call_id, self.json_.data(),
                              encoded_bytes)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    self.completion_ = completion;
    self.attempt_ = request->attempt;
    self.call_ = request->logical_call_id;
    self.deadline_ = request->deadline_us;
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
            result.result = {answer_.data(), uint32_t(answer_size_)};
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
            const auto rc = model_tools_.decode({p.body.data(), p.size});
            model_ = rc == AYNI_OK ? ModelState::tools : ModelState::failed;
            if (rc != AYNI_OK) {
                status(turn_, "model tool group invalid or over capacity; no tools dispatched");
            }
        } else if (p.kind == Kind::model_text && p.size <= answer_bytes) {
            std::memcpy(answer_.data(), p.body.data(), p.size);
            answer_size_ = p.size;
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
        if (!announced_ || goal_ || pending_command_ || closing_ != Closing::open || !p.size ||
            p.size > prompt_bytes) {
            status(p.id, "input refused: not ready/busy/over limit");
            return;
        }
        auto body = sized<ayni_goal_create_v1>();
        body.goal_type_version = version;
        body.goal_type_id = text(strategy_id);
        body.owner_instance_id = instance_;
        body.parameters = {p.body.data(), p.size};
        body.trigger = AYNI_TRIGGER_ONCE;
        const auto rc = command(AYNI_INTENT_GOAL_CREATE, &body, &goal_);
        if (rc == AYNI_OK) {
            turn_ = p.id;
            hold_reported_ = false;
            status(turn_, "accepted");
        } else {
            char error[64];
            std::snprintf(error, sizeof(error), "input error=%ld", long(rc));
            status(p.id, error);
        }
    } else if (p.kind == Kind::cancel && goal_) {
        // Owner command backpressure must not silently discard a user's cancellation.
        cancel_requested_ = true;
        status(p.id, "cancel queued; waiting for current command receipt");
    } else if (p.kind == Kind::shutdown && closing_ == Closing::open) {
        closing_ = Closing::cancel_work;
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
    append("session metadata (no message bodies):");
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
ayni_status_t Chat::tick() {
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
    auto rc = ayni_step(kernel_, step_units, step_us, nullptr);
    auto info = sized<ayni_kernel_info_v1>();
    if (rc == AYNI_OK) {
        rc = ayni_get_kernel_info(kernel_, &info);
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
            heap_caps_free(value_workspace_.data);
            value_workspace_ = {};
        }
        status(0, "stopped; reset board before reconnecting");
        return AYNI_NOT_READY;
    }
    if (info.lifecycle == AYNI_LIFECYCLE_FENCED || info.lifecycle == AYNI_LIFECYCLE_RESTRICTED) {
        return AYNI_NOT_READY;
    }
    if (info.lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_OK;
    }
    if (!session_) {
        const ayni_session_desc_v1 desc{sizeof(desc), AYNI_CHANNEL_MANAGEMENT, principal, version,
                                        0};
        rc = ayni_open_session(kernel_, &desc, &session_);
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
    }
    rc = receipt();
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
        }
        return rc;
    }
    constexpr const char *grant_capabilities[] = {capability_id,    "screen.text",  "speaker.tone",
                                                  "speaker.speech", "motor.status", "motor.plan",
                                                  "motor.run",      "motor.stop"};
    if (grant_ < sizeof(grant_capabilities) / sizeof(grant_capabilities[0])) {
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
                return AYNI_OK;
            }
        }
        const bool motion = grant_ == motor_run_grant_index;
        const ayni_grant_create_v1 grant{sizeof(grant),
                                         version,
                                         text(grant_capabilities[grant_]),
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
    if (!announced_ && !pending_command_) {
        bool settled = false;
        rc = settle_session(info.durable_seq, settled);
        if (rc != AYNI_OK || !settled) {
            return rc;
        }
        announced_ = true;
        speaker_.set_accepting(true);
        motor_.execution().set_accepting(true);
        diagnostics();
        status(0, "ready");
    }
    if (cancel_requested_ && goal_ && !pending_command_) {
        auto goal = sized<ayni_goal_info_v1>();
        rc = ayni_get_goal(kernel_, goal_, &goal);
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
    if (goal_) {
        auto goal = sized<ayni_goal_info_v1>();
        rc = ayni_get_goal(kernel_, goal_, &goal);
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
        if (!goal.last_task_id && goal.commit_seq <= info.durable_seq &&
            (goal.lifecycle == AYNI_GOAL_CANCELLED || goal.lifecycle == AYNI_GOAL_FAILED)) {
            status(turn_, "turn failed/cancelled before task creation; no automatic retry");
            goal_ = turn_ = 0;
            cancel_requested_ = false;
        } else if (goal.last_task_id) {
            auto task = sized<ayni_task_info_v1>();
            rc = ayni_get_task(kernel_, goal.last_task_id, &task);
            if (rc != AYNI_OK) {
                return rc;
            }
            if (task.lifecycle == AYNI_TASK_COMPLETED && task.commit_seq <= info.durable_seq) {
                uint32_t bytes = 0;
                rc = ayni_copy_task_checkpoint(kernel_, task.task_id,
                                               reinterpret_cast<uint8_t *>(checkpoint_.data()),
                                               checkpoint_.size(), &bytes);
                if (rc != AYNI_OK) {
                    return rc;
                }
                if (bytes >= 12 &&
                    transport_.send(Kind::answer, turn_, checkpoint_.data() + 12, bytes - 12)) {
                    goal_ = turn_ = 0;
                    cancel_requested_ = false;
                }
            } else if (task.lifecycle == AYNI_TASK_FAILED ||
                       task.lifecycle == AYNI_TASK_CANCELLED) {
                if (task.commit_seq <= info.durable_seq) {
                    status(turn_, "turn failed/cancelled; no automatic retry");
                    goal_ = turn_ = 0;
                    cancel_requested_ = false;
                }
            } else if (task.lifecycle == AYNI_TASK_PAUSED && !hold_reported_) {
                char message[96];
                std::snprintf(message, sizeof(message), "task held status=%ld reason=%lu",
                              long(task.hold_status), static_cast<unsigned long>(task.hold_reason));
                status(turn_, message);
                hold_reported_ = true;
            }
        }
    }
    return AYNI_OK;
}
} // namespace usbchat
