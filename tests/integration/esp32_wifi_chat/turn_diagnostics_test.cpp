// Diagnostic breakdown checks for ESP32 Wi-Fi Chat publication, model handoff and storage metrics.
#include "configuration.h"
#include "turn_diagnostics.h"
#include <ayni/ayni.hpp>
#include <ayni/commands.hpp>
#include <ayni/context.h>
#include <ayni/host_authority.h>
#include <ayni/hosts/sim.h>
#include <ayni/model_messages.hpp>
#include <ayni/runtime/conversation.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/strategies/builtin.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace {
using ayni::initialized;
using ayni::hosts::esp_idf::StorageMetrics;
using ayni::hosts::storage::FileValueRequest;
using ayni::hosts::storage::FileValueResult;
using Stage = wifichat::PublicationTimeline::Stage;
using Order = wifichat::PublicationTimeline::Order;
using Overlap = wifichat::UiFrameWindow::Overlap;

ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}

// Asynchronous in-memory FileValue worker mirroring ESP's queued read/write/remove accounting.
class QueuedValueStore {
  public:
    explicit QueuedValueStore(ayni::hosts::sim::Host &host) : host_(host) {}
    ayni::hosts::storage::FileValuePort port() { return {this, submit}; }
    bool empty() const { return pending_.empty(); }
    const StorageMetrics &metrics() const { return metrics_; }

    void advance() {
        if (pending_.empty()) {
            return;
        }
        const auto item = pending_.front();
        pending_.pop_front();
        const auto &r = item.request;
        const auto started = host_.now_us();
        const auto key = std::make_pair(r.key.owner_instance_id, r.key.object_id);
        FileValueResult result{AYNI_OK, 0, 0, 0};
        uint64_t service_us = 0;
        auto found = values_.find(key);
        if (r.kind == FileValueRequest::Kind::remove) {
            service_us = 12000;
            values_.erase(key);
            ++metrics_.value_remove_calls;
            metrics_.value_remove_us += service_us;
        } else if (r.kind == FileValueRequest::Kind::write) {
            service_us = 18000;
            values_[key] = {r.generation, {r.data, r.data + r.size}};
            result.generation = r.generation;
            result.size = r.size;
            ++metrics_.value_write_calls;
            metrics_.value_write_us += service_us;
            ++writes_since_measure_;
        } else {
            service_us = 6000;
            ++metrics_.value_read_calls;
            metrics_.value_read_us += service_us;
            if (found == values_.end()) {
                result.status = AYNI_NOT_FOUND;
            } else if (r.size < found->second.bytes.size()) {
                result.status = AYNI_BUFFER_TOO_SMALL;
            } else {
                std::memcpy(r.buffer, found->second.bytes.data(), found->second.bytes.size());
                result.generation = found->second.generation;
                result.size = static_cast<uint32_t>(found->second.bytes.size());
                ++metrics_.files.read_calls;
                metrics_.files.read_bytes += result.size;
                metrics_.files.read_us += service_us;
            }
        }
        host_.advance_us(service_us);
        ++metrics_.completed;
        metrics_.queue_us += started - item.submitted_us;
        metrics_.io_us += service_us;
        metrics_.value_us += service_us;
        // Simulate periodic lfs_fs_size() reconciliation every 4 writes (like SpaceLedger
        // threshold).
        if (writes_since_measure_ >= 4U) {
            constexpr uint64_t measure_us = 25000;
            writes_since_measure_ = 0;
            host_.advance_us(measure_us);
            metrics_.space_us += measure_us;
            ++metrics_.files.space_measure_calls;
            metrics_.files.space_measure_us += measure_us;
        }
        r.complete(r.context, result);
    }

  private:
    struct Value {
        uint64_t generation = 0;
        std::vector<uint8_t> bytes;
    };
    struct Queued {
        FileValueRequest request;
        uint64_t submitted_us = 0;
    };
    static ayni_submit_result_t submit(void *ctx, const FileValueRequest *request) {
        auto &self = *static_cast<QueuedValueStore *>(ctx);
        if (self.pending_.size() >= 8U) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.pending_.push_back({*request, self.host_.now_us()});
        return AYNI_SUBMIT_ACCEPTED;
    }
    ayni::hosts::sim::Host &host_;
    std::map<std::pair<uint64_t, uint64_t>, Value> values_;
    std::deque<Queued> pending_;
    StorageMetrics metrics_{};
    uint32_t writes_since_measure_ = 0;
};

struct FixedTextModel {
    ayni::hosts::sim::Host *host = nullptr;
    const ayni_body_store_v1 *bodies = nullptr;
    ayni_provider_completion_t *completion = nullptr;
    ayni_attempt_ref_v1 attempt{};
    uint64_t call_id = 0;
    std::string reply;
    uint64_t last_posted_us = 0;
    uint64_t last_posted_call = 0;

    static ayni_submit_result_t submit(void *ctx, const ayni_provider_request_v1 *request,
                                       ayni_provider_completion_t *completion) {
        auto &self = *static_cast<FixedTextModel *>(ctx);
        if (self.completion) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.completion = completion;
        self.attempt = request->attempt;
        self.call_id = request->logical_call_id;
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    bool deliver() {
        if (!completion) {
            return false;
        }
        std::string framed;
        const ayni::model::Writer::Grow grow = [](void *context, uint32_t size) {
            auto &value = *static_cast<std::string *>(context);
            value.resize(size);
            return value.data();
        };
        ayni::model::Writer writer(&framed, grow, 0);
        ayni::model::write_reply_frame(writer, {reply.data(), static_cast<uint32_t>(reply.size())},
                                       {});
        ayni_string_v1 encoded{};
        EXPECT_TRUE(writer.finish(encoded));
        ayni_body_ref_v1 body{};
        const auto staged =
            bodies->stage(bodies->context, call_id,
                          {reinterpret_cast<const uint8_t *>(encoded.data), encoded.size}, &body);
        if (staged == AYNI_NOT_READY) {
            return false;
        }
        EXPECT_EQ(staged, AYNI_OK);
        auto result = initialized<ayni_provider_result_v1>();
        result.attempt = attempt;
        result.outcome = AYNI_CALL_COMPLETED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        result.body = body;
        if (ayni_provider_post(completion, &result) == AYNI_OK) {
            last_posted_us = host->now_us();
            last_posted_call = call_id;
            completion = nullptr;
            return true;
        }
        return false;
    }
};

struct EndToEndSample {
    wifichat::PublicationTimeline::Summary summary;
    wifichat::StoragePhaseDelta storage;
    uint64_t call_id = 0;
    uint64_t model_posted_us = 0, answer_saved_us = 0;
    uint32_t stage_callbacks = 0;
    uint32_t confirm_busy_hits = 0, collect_busy_hits = 0;
};

EndToEndSample run_fixed_text_turn(ayni::strategies::Persistence persistence) {
    constexpr uint64_t owner = 1, agent_id = 1;
    constexpr uint32_t version = 1, step_items = 16;
    constexpr uint64_t step_us = 4000, tick_us = 2000;
    constexpr char chat_type[] = "wifi.chat", tools_cap[] = "wifi.model.tools",
                   reply_cap[] = "wifi.model.text", tools_bind[] = "wifi.tools",
                   reply_bind[] = "wifi.model";

    ayni::hosts::sim::Host host(1000000);
    QueuedValueStore values(host);
    ayni::runtime::FileBodies bodies;
    EXPECT_EQ(bodies.configure(values.port(), 512U * 1024U), AYNI_OK);
    bodies.set_shared_call_results();

    size_t ws_bytes = 0, ws_align = 1;
    for (const auto query : {ayni::strategies::ToolLoop::workspace_requirements,
                             ayni::strategies::Conversation::workspace_requirements}) {
        size_t sz = 0, al = 0;
        EXPECT_EQ(query(&sz, &al), AYNI_OK);
        ws_bytes = std::max(ws_bytes, sz);
        ws_align = std::max(ws_align, al);
    }
    EXPECT_LE(ws_align, alignof(std::max_align_t));
    std::vector<uint64_t> ws_storage((ws_bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
    ayni_workspace_v1 workspace{sizeof(workspace), 0, ws_storage.data(),
                                ws_storage.size() * sizeof(uint64_t)};

    const ayni_string_v1 route = text("model.fixed00000000001");
    ayni::ValueBuilder<3> input_shape;
    input_shape.object();
    input_shape.identifier(AYNI_MODEL_DESTINATION_FIELD_ID, route);
    input_shape.body(AYNI_MODEL_BODY_FIELD_ID, {1, 0, 0, 0});
    input_shape.end();
    ayni_value_v1 shaped{};
    EXPECT_EQ(input_shape.finish(shaped), AYNI_OK);
    const uint32_t input_bytes = ayni::encoded_bytes(shaped);
    auto config = wifichat::chat_config(input_bytes, wifichat::summary_bytes);

    auto ports = host.table();
    ports.bodies = bodies.store();
    ayni::Kernel kernel;
    const ayni::Quiesce quiesce{
        &host, [](void *c) -> ayni_status_t {
            return static_cast<ayni::hosts::sim::Host *>(c)->pending_storage() == 0 ? AYNI_OK
                                                                                    : AYNI_BUSY;
        }};
    EXPECT_EQ(kernel.create(config, ports, quiesce), AYNI_OK);

    const ayni_principal_desc_v1 principal{sizeof(principal), AYNI_PRINCIPAL_OWNER, owner, version,
                                           0};
    const ayni_agent_desc_v1 agent{sizeof(agent), 0, agent_id};
    EXPECT_EQ(ayni_register_principal(kernel.get(), &principal), AYNI_OK);
    EXPECT_EQ(ayni_register_agent(kernel.get(), &agent), AYNI_OK);

    auto domain = initialized<ayni_authority_value_set_v1>();
    domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    domain.values.tokens = {&route, 1, 0};
    const ayni_authority_field_desc_v1 auth_field{sizeof(auth_field),
                                                  AYNI_MODEL_DESTINATION_FIELD_ID,
                                                  AYNI_AUTHORITY_EGRESS_DESTINATION, 0, domain};
    for (const char *cid : {reply_cap, tools_cap}) {
        const ayni_capability_desc_v1 cap{sizeof(cap), version, text(cid), 1, 0, &auth_field};
        EXPECT_EQ(ayni_register_capability(kernel.get(), &cap), AYNI_OK);
    }

    const ayni_call_resource_v1 call_resource{text("model.slot"), 1, 0};
    std::array<ayni_schema_node_v1, 3> input_schema{};
    input_schema[0].kind = AYNI_VALUE_OBJECT;
    input_schema[0].subtree_end = uint32_t(input_schema.size());
    input_schema[1].kind = AYNI_VALUE_IDENTIFIER;
    input_schema[1].field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    input_schema[1].subtree_end = 2;
    input_schema[1].name = text("destination");
    input_schema[1].flags = AYNI_SCHEMA_REQUIRED;
    input_schema[1].choice_count = 1;
    input_schema[1].choices = &route;
    input_schema[2].kind = AYNI_VALUE_TEXT;
    input_schema[2].field_id = AYNI_MODEL_BODY_FIELD_ID;
    input_schema[2].subtree_end = uint32_t(input_schema.size());
    input_schema[2].name = text("body");
    input_schema[2].flags = AYNI_SCHEMA_REQUIRED | AYNI_SCHEMA_EXTERNAL;
    input_schema[2].max_count = wifichat::harness::model_body_bytes;
    ayni_schema_node_v1 result_schema{};
    result_schema.kind = AYNI_VALUE_TEXT;
    result_schema.subtree_end = 1;
    result_schema.max_count = wifichat::summary_bytes;
    ayni_authority_projection_v1 projection{};
    projection.field_id = AYNI_MODEL_DESTINATION_FIELD_ID;
    projection.path_count = 1;
    projection.path[0] = AYNI_MODEL_DESTINATION_FIELD_ID;
    ayni_business_contract_v1 business{{input_schema.data(), uint32_t(input_schema.size())},
                                       {&result_schema, 1},
                                       1,
                                       0,
                                       &projection};

    FixedTextModel model{&host, bodies.store(), nullptr, {}, 0, "固定历史回复。", 0, 0};
    std::array<ayni_call_capability_v1, 2> caps{};
    for (uint32_t at = 0; at < caps.size(); ++at) {
        const bool tools = at == 1;
        caps[at] = {sizeof(ayni_call_capability_v1),
                    version,
                    text(tools ? tools_cap : reply_cap),
                    {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL,
                     AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING, AYNI_CALL_NOT_IDEMPOTENT,
                     AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                     wifichat::harness::request_timeout_ms, AYNI_CALL_INPUT_VALUE,
                     tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT, input_bytes,
                     static_cast<uint32_t>(wifichat::summary_bytes), 1, &call_resource, nullptr},
                    &business};
        EXPECT_EQ(ayni_register_call_capability(kernel.get(), &caps[at]), AYNI_OK);
    }
    ayni_provider_v1 provider{sizeof(provider),
                              version,
                              route,
                              text("wifi.endpoint"),
                              route,
                              &model,
                              uint32_t(caps.size()),
                              0,
                              caps.data(),
                              FixedTextModel::submit,
                              FixedTextModel::cancel,
                              nullptr,
                              nullptr};
    EXPECT_EQ(ayni_register_provider(kernel.get(), &provider), AYNI_OK);
    const ayni_resource_desc_v1 resource{
        sizeof(resource), AYNI_RESOURCE_EXCLUSIVE, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
    EXPECT_EQ(ayni_register_resource(kernel.get(), &resource), AYNI_OK);
    EXPECT_EQ(ayni_set_resource_health(kernel.get(), resource.resource_id, AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    for (uint32_t at = 0; at < caps.size(); ++at) {
        const ayni_binding_v1 b{sizeof(b),
                                version,
                                text(at == 0 ? reply_bind : tools_bind),
                                caps[at].capability_id,
                                version,
                                version,
                                provider.provider_id,
                                provider.endpoint_id,
                                route,
                                0,
                                0,
                                nullptr};
        EXPECT_EQ(ayni_register_binding(kernel.get(), &b), AYNI_OK);
    }

    const ayni_scope_field_v1 scope_field{sizeof(scope_field), AYNI_MODEL_DESTINATION_FIELD_ID,
                                          domain};
    const ayni_scope_v1 destination_scope{sizeof(destination_scope), 1, &scope_field};
    ayni::strategies::ToolLoopConfig loop_cfg;
    loop_cfg.identity.id = text(chat_type);
    loop_cfg.model.kind = loop_cfg.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    loop_cfg.model.binding_id = text(tools_bind);
    loop_cfg.summary.binding_id = text(reply_bind);
    loop_cfg.model.arguments = loop_cfg.summary.arguments = destination_scope;
    loop_cfg.model.text = text("你是Ayni，一个通用助手。");
    loop_cfg.limits = {
        wifichat::harness::context_window_tokens, wifichat::harness::max_output_tokens,
        wifichat::harness::compact_target_tokens, wifichat::harness::tool_result_max_tokens};
    loop_cfg.reply_encoding = loop_cfg.summary_reply_encoding = ayni::model::ReplyEncoding::framed;
    loop_cfg.bodies = bodies.store();
    loop_cfg.body_progress = {&bodies, ayni::runtime::FileBodies::progress};
    loop_cfg.persistence = persistence;
    ayni::strategies::ToolLoop loop;
    EXPECT_EQ(loop.configure(loop_cfg, &workspace), AYNI_OK);
    auto strategy = loop.describe();
    EXPECT_EQ(ayni_register_strategy(kernel.get(), &strategy), AYNI_OK);
    auto type = initialized<ayni_goal_type_v1>();
    type.version = version;
    type.goal_type_id = text(chat_type);
    type.strategy_id = strategy.strategy_id;
    type.ctx = &workspace;
    type.validate = ayni::strategies::Conversation::validate_turn;
    type.export_requirements = [](void *, ayni_bytes_v1,
                                  const ayni_goal_requirement_sink_v1 *sink) {
        for (const char *cid : {"wifi.model.tools", "wifi.model.text"}) {
            const ayni_goal_requirement_v1 req{
                sizeof(req), 1, text(cid), {sizeof(ayni_scope_v1), 0, nullptr}};
            const auto rc = sink->emit(sink->ctx, &req);
            if (rc != AYNI_OK) {
                return rc;
            }
        }
        return AYNI_OK;
    };
    type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.context_mode = AYNI_GOAL_CONTEXT_AGENT;
    EXPECT_EQ(ayni_register_goal_type(kernel.get(), &type), AYNI_OK);
    EXPECT_EQ(kernel.start(), AYNI_OK);

    uint64_t session_id = 0, instance_id = 0;
    ayni::CommandClient commands;
    ayni::runtime::conversation::Session session;
    bool session_bound = false;
    StorageMetrics journal_metrics{};

    auto step_all = [&] {
        auto report = initialized<ayni_step_report_v1>();
        EXPECT_EQ(kernel.step(step_items, step_us, report), AYNI_OK);
        if (commands.pending()) {
            EXPECT_EQ(commands.poll(), AYNI_OK);
        }
        if (session_bound) {
            const auto s = session.poll();
            EXPECT_TRUE(s == AYNI_OK || s == AYNI_BUSY || s == AYNI_NOT_READY);
        }
        model.deliver();
        if (host.pending_storage() > 0U) {
            constexpr uint64_t append_us = 15000;
            const auto pending = host.pending_storage();
            host.advance_us(append_us);
            host.complete_storage();
            journal_metrics.completed += pending;
            journal_metrics.append_calls += pending;
            journal_metrics.append_us += append_us * pending;
            journal_metrics.io_us += append_us * pending;
        }
        values.advance();
        host.advance_us(tick_us);
    };
    auto until = [&](auto done) {
        for (uint32_t i = 0; i < 4000; ++i) {
            if (done()) {
                return true;
            }
            step_all();
        }
        return done();
    };

    const ayni_session_desc_v1 sdesc{sizeof(sdesc), AYNI_CHANNEL_MANAGEMENT, owner, version, 0};
    EXPECT_TRUE(
        until([&] { return ayni_open_session(kernel.get(), &sdesc, &session_id) == AYNI_OK; }));
    auto ainfo = initialized<ayni_agent_info_v1>();
    EXPECT_EQ(ayni_get_agent(kernel.get(), agent_id, &ainfo), AYNI_OK);
    instance_id = ainfo.instance_id;
    auto kinfo = initialized<ayni_kernel_info_v1>();
    EXPECT_EQ(ayni_get_kernel_info(kernel.get(), &kinfo), AYNI_OK);
    EXPECT_EQ(commands.configure(kernel.get(), owner, session_id, kinfo.boot_epoch), AYNI_OK);

    for (const char *cid : {tools_cap, reply_cap}) {
        EXPECT_TRUE(until([&] { return !commands.pending(); }));
        const ayni_grant_create_v1 g{sizeof(g),   version, text(cid),
                                     instance_id, 0,       AYNI_GRANT_PERMANENT,
                                     0,           0,       {sizeof(ayni_scope_v1), 0, nullptr}};
        EXPECT_EQ(commands.submit(AYNI_INTENT_GRANT_CREATE, &g).status, AYNI_OK);
    }
    EXPECT_TRUE(until([&] { return !commands.pending(); }));
    auto open_ctx = initialized<ayni_context_command_v1>();
    open_ctx.owner_instance_id = instance_id;
    EXPECT_EQ(commands.submit(AYNI_INTENT_CONTEXT_OPEN, &open_ctx).status, AYNI_OK);
    EXPECT_TRUE(until([&] { return !commands.pending(); }));

    struct RetryWrapper {
        const ayni_body_store_v1 *inner_store = nullptr;
        ayni::runtime::BodyCollection inner_col{};
        ayni_body_store_v1 store{};
        uint32_t confirm_busy_remaining = 0, confirm_busy_hits = 0;
        uint32_t collect_busy_remaining = 0, collect_busy_hits = 0;
    } retry;
    retry.inner_store = bodies.store();
    retry.inner_col = bodies.collection();
    retry.store = *retry.inner_store;
    retry.store.context = &retry;
    retry.store.write = [](void *c, uint64_t sc, ayni_bytes_v1 b, ayni_body_ref_v1 *o) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_store->write(r.inner_store->context, sc, b, o);
    };
    retry.store.read = [](void *c, const ayni_body_ref_v1 *ref, uint8_t *o, uint32_t cap) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_store->read(r.inner_store->context, ref, o, cap);
    };
    retry.store.release = [](void *c, const ayni_body_ref_v1 *ref) {
        auto &r = *static_cast<RetryWrapper *>(c);
        r.inner_store->release(r.inner_store->context, ref);
    };
    retry.store.stage = [](void *c, uint64_t sc, ayni_bytes_v1 b, ayni_body_ref_v1 *o) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_store->stage(r.inner_store->context, sc, b, o);
    };
    retry.store.share = [](void *c, uint64_t sc, const ayni_body_ref_v1 *ref, ayni_body_ref_v1 *o) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_store->share(r.inner_store->context, sc, ref, o);
    };
    retry.store.confirm = [](void *c, const ayni_body_ref_v1 *ref) {
        auto &r = *static_cast<RetryWrapper *>(c);
        if (r.confirm_busy_remaining > 0U) {
            --r.confirm_busy_remaining;
            ++r.confirm_busy_hits;
            return AYNI_NOT_READY;
        }
        return r.inner_store->confirm(r.inner_store->context, ref);
    };
    auto collection = retry.inner_col;
    collection.store = &retry.store;
    collection.context = &retry;
    collection.collect = [](void *c, uint64_t sc, const std::vector<ayni_body_ref_v1> &keep) {
        auto &r = *static_cast<RetryWrapper *>(c);
        r.inner_col.collect(r.inner_col.context, sc, keep);
    };
    collection.ready = [](void *c, uint64_t task) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_col.ready(r.inner_col.context, task);
    };
    collection.progress = [](void *c, uint64_t task) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_col.progress(r.inner_col.context, task);
    };
    collection.status = [](void *c) {
        auto &r = *static_cast<RetryWrapper *>(c);
        return r.inner_col.status(r.inner_col.context);
    };
    collection.collected = [](void *c, uint64_t sc) {
        auto &r = *static_cast<RetryWrapper *>(c);
        if (r.collect_busy_remaining > 0U) {
            --r.collect_busy_remaining;
            ++r.collect_busy_hits;
            return AYNI_BUSY;
        }
        return r.inner_col.collected(r.inner_col.context, sc);
    };

    struct ObserverCtx {
        ayni::hosts::sim::Host *host = nullptr;
        wifichat::PublicationTimeline timeline;
        bool armed = false;
        uint32_t callbacks = 0;
    } obs{&host, {}, false, 0};

    ayni::runtime::conversation::Config scfg;
    scfg.kernel = kernel.get();
    scfg.session = session_id;
    scfg.owner_instance = instance_id;
    scfg.goal_type = text(chat_type);
    scfg.workspace = &workspace;
    scfg.files = values.port();
    scfg.bodies = collection;
    scfg.commands = {&commands, [](void *c, ayni_intent_kind_t k, const void *b, uint32_t rev) {
                         return static_cast<ayni::CommandClient *>(c)->submit(k, b, rev);
                     }};
    scfg.unfinished = ayni::runtime::conversation::UnfinishedTurn::discard;
    scfg.persistence = persistence;
    scfg.observer = {&obs, [](void *c, uint64_t g, uint64_t t, Stage st) {
                         auto &o = *static_cast<ObserverCtx *>(c);
                         if (!o.armed) {
                             return;
                         }
                         ++o.callbacks;
                         wifichat::PublicationTimeline::Step step{};
                         EXPECT_TRUE(o.timeline.record_stage(g, t, st, o.host->now_us(), step));
                     }};
    EXPECT_EQ(session.configure(scfg), AYNI_OK);
    session_bound = true;
    EXPECT_TRUE(until([&] { return session.ready(); }));

    // Turn 1: fixed initial history seed.
    EXPECT_EQ(session.begin(text("固定初始历史问题")), AYNI_OK);
    ayni::runtime::conversation::TurnResult seed{};
    EXPECT_TRUE(until([&] { return session.take_result(seed); }));
    EXPECT_TRUE(seed.published);
    EXPECT_EQ(session.record()->round_count, 1U);

    // Turn 2: fixed text input & reply, armed observer + deterministic BUSY/NOT_READY retries.
    model.reply = "固定最终诊断回复正文，用于核对发布分段耗时。";
    obs.timeline.reset(2);
    obs.armed = true;
    retry.confirm_busy_remaining = 2;
    retry.collect_busy_remaining = 3;
    const uint64_t prev_call = model.last_posted_call;
    EXPECT_EQ(session.begin(text("固定文本输入")), AYNI_OK);
    EXPECT_TRUE(until([&] { return model.last_posted_call != prev_call; }));
    const uint64_t model_posted_us = model.last_posted_us;
    obs.timeline.mark_model_posted(model_posted_us);

    auto combined_metrics = [&] {
        auto m = values.metrics();
        m.completed += journal_metrics.completed;
        m.append_calls += journal_metrics.append_calls;
        m.append_us += journal_metrics.append_us;
        m.io_us += journal_metrics.io_us;
        return m;
    };
    const auto metrics_before = combined_metrics();

    ayni::runtime::conversation::TurnResult turn_result{};
    EXPECT_TRUE(until([&] { return session.take_result(turn_result); }));
    EXPECT_TRUE(turn_result.published);
    std::string answer;
    EXPECT_TRUE(until([&] {
        return ayni::strategies::Conversation::latest_answer(*bodies.store(), *session.record(),
                                                             answer) == AYNI_OK;
    }));
    const uint64_t answer_saved_us = host.now_us();
    EXPECT_EQ(answer, model.reply);
    wifichat::PublicationTimeline::Step final_step{};
    EXPECT_TRUE(obs.timeline.record_answer_saved(answer_saved_us, final_step));

    const auto metrics_after = combined_metrics();
    EndToEndSample out{};
    out.summary = obs.timeline.summary();
    out.storage = wifichat::StoragePhaseDelta::between(metrics_before, metrics_after,
                                                       answer_saved_us - model_posted_us);
    out.call_id = model.last_posted_call;
    out.model_posted_us = model_posted_us;
    out.answer_saved_us = answer_saved_us;
    out.stage_callbacks = obs.callbacks;
    out.confirm_busy_hits = retry.confirm_busy_hits;
    out.collect_busy_hits = retry.collect_busy_hits;

    EXPECT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
    session_bound = false;
    EXPECT_EQ(ayni_begin_shutdown(kernel.get()), AYNI_OK);
    EXPECT_TRUE(until([&] {
        auto ki = initialized<ayni_kernel_info_v1>();
        return ayni_get_kernel_info(kernel.get(), &ki) == AYNI_OK &&
               ki.lifecycle == AYNI_LIFECYCLE_STOPPED && values.empty() && bodies.idle();
    }));
    EXPECT_EQ(kernel.close(), AYNI_OK);
    return out;
}

TEST(TurnDiagnostics, PublicationTimelineAsyncOrderRecordsEachBoundaryOnceAndSumsToTotal) {
    wifichat::PublicationTimeline timeline;
    timeline.reset(77);
    // Multi-round turn: earlier tool round is overwritten by the final model_result_posted.
    timeline.mark_model_posted(100000);
    timeline.mark_model_posted(200000);

    wifichat::PublicationTimeline::Step step{};
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::checkpoint_ready, 212000, step));
    EXPECT_EQ(step.seg_us, 12000U);
    EXPECT_EQ(step.elapsed_us, 12000U);
    // Duplicate/retry callbacks do not overwrite or double-emit a boundary.
    EXPECT_FALSE(timeline.record_stage(11, 22, Stage::checkpoint_ready, 215000, step));
    // A late model_posted mark cannot disturb an in-progress publication.
    timeline.mark_model_posted(999999);

    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::publish_durable, 290000, step));
    EXPECT_EQ(step.seg_us, 78000U);
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::record_saved, 450000, step));
    EXPECT_EQ(step.seg_us, 160000U);
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::collected, 1100000, step));
    EXPECT_EQ(step.seg_us, 650000U);
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::context_ready, 1240000, step));
    EXPECT_EQ(step.seg_us, 140000U);
    ASSERT_TRUE(timeline.record_answer_saved(1245000, step));
    EXPECT_EQ(step.seg_us, 5000U);
    EXPECT_EQ(step.elapsed_us, 1045000U);
    EXPECT_FALSE(timeline.record_answer_saved(1300000, step));

    const auto summary = timeline.summary();
    ASSERT_TRUE(summary.complete);
    EXPECT_EQ(summary.order, Order::async);
    EXPECT_EQ(summary.turn, 77U);
    EXPECT_EQ(summary.goal, 11U);
    EXPECT_EQ(summary.task, 22U);
    EXPECT_EQ(summary.total_us, 1045000U);
    const std::array<uint64_t, 6> expected{12000, 78000, 160000, 650000, 140000, 5000};
    EXPECT_EQ(summary.segments_us, expected);
    EXPECT_EQ(std::accumulate(summary.segments_us.begin(), summary.segments_us.end(), uint64_t{0}),
              summary.total_us);
}

TEST(TurnDiagnostics, PublicationTimelineDirectOrderPreservesPublishBeforeCheckpoint) {
    wifichat::PublicationTimeline timeline;
    timeline.reset(88);
    timeline.mark_model_posted(500000);

    wifichat::PublicationTimeline::Step step{};
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::publish_durable, 560000, step));
    EXPECT_EQ(step.seg_us, 60000U);
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::checkpoint_ready, 565000, step));
    EXPECT_EQ(step.seg_us, 5000U);
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::record_saved, 715000, step));
    EXPECT_EQ(step.seg_us, 150000U);
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::collected, 915000, step));
    EXPECT_EQ(step.seg_us, 200000U);
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::context_ready, 980000, step));
    EXPECT_EQ(step.seg_us, 65000U);
    ASSERT_TRUE(timeline.record_answer_saved(984000, step));
    EXPECT_EQ(step.seg_us, 4000U);

    const auto summary = timeline.summary();
    ASSERT_TRUE(summary.complete);
    EXPECT_EQ(summary.order, Order::direct);
    EXPECT_EQ(summary.total_us, 484000U);
    const std::array<uint64_t, 6> expected{60000, 5000, 150000, 200000, 65000, 4000};
    EXPECT_EQ(summary.segments_us, expected);
    EXPECT_EQ(std::accumulate(summary.segments_us.begin(), summary.segments_us.end(), uint64_t{0}),
              summary.total_us);
}

TEST(TurnDiagnostics, UiFrameWindowClassifiesAllOverlapsAndHandoffAdditivity) {
    const wifichat::UiFrameWindow frame{1000000, 1180000, 1180000, 1215000};
    uint64_t wait_us = 0;

    EXPECT_EQ(frame.classify(1220000, wait_us), Overlap::idle);
    EXPECT_EQ(wait_us, 0U);

    EXPECT_EQ(frame.classify(998000, wait_us), Overlap::before_draw);
    EXPECT_EQ(wait_us, 215000U);

    EXPECT_EQ(frame.classify(1050000, wait_us), Overlap::in_draw);
    EXPECT_EQ(wait_us, 165000U);

    EXPECT_EQ(frame.classify(1190000, wait_us), Overlap::in_paint);
    EXPECT_EQ(wait_us, 25000U);

    // 5-point model handoff correlation: http -> done -> ui -> recv -> stage -> post.
    constexpr uint64_t http_us = 1038000, done_us = 1050000, ui_us = 1225000;
    constexpr uint64_t recv_us = 1227000, stage_us = 1230000, post_us = 1231000;
    const uint64_t pack_us = done_us - http_us;
    const uint64_t ui_wait_us = ui_us - done_us;
    const uint64_t owner_wait_us = recv_us - ui_us;
    const uint64_t stage_wait_us = stage_us - recv_us;
    const uint64_t post_wait_us = post_us - stage_us;
    EXPECT_EQ(pack_us + ui_wait_us + owner_wait_us + stage_wait_us + post_wait_us,
              post_us - http_us);
    EXPECT_EQ(frame.classify(done_us, wait_us), Overlap::in_draw);
    EXPECT_LE(wait_us, ui_wait_us);
}

TEST(TurnDiagnostics, StoragePhaseDeltaSeparatesOperationTypesAndActualLfsSize) {
    StorageMetrics before{};
    before.completed = 10;
    before.queue_us = 50000;
    before.io_us = 100000;
    before.space_us = 4000;
    before.append_calls = 3;
    before.append_us = 30000;
    before.value_us = 70000;
    before.value_read_calls = 1;
    before.value_read_us = 5000;
    before.value_write_calls = 4;
    before.value_write_us = 50000;
    before.value_remove_calls = 2;
    before.value_remove_us = 15000;
    before.files.space_measure_calls = 1;
    before.files.space_measure_us = 3500;

    StorageMetrics after = before;
    after.completed += 9;
    after.queue_us += 900000; // Concurrent queue waits can exceed phase wall_us.
    after.io_us += 620000;
    after.space_us += 52000;
    after.append_calls += 2;
    after.append_us += 120000;
    after.value_us += 500000;
    after.value_read_calls += 1;
    after.value_read_us += 20000;
    after.value_write_calls += 3;
    after.value_write_us += 280000;
    after.value_remove_calls += 3;
    after.value_remove_us += 200000;
    after.files.space_measure_calls += 1;
    after.files.space_measure_us += 49000;

    const auto delta = wifichat::StoragePhaseDelta::between(before, after, 700000);
    EXPECT_EQ(delta.wall_us, 700000U);
    EXPECT_EQ(delta.completed, 9U);
    EXPECT_EQ(delta.queue_us, 900000U);
    EXPECT_EQ(delta.io_us, 620000U);
    EXPECT_EQ(delta.append_calls, 2U);
    EXPECT_EQ(delta.append_us, 120000U);
    EXPECT_EQ(delta.value_read_calls, 1U);
    EXPECT_EQ(delta.value_read_us, 20000U);
    EXPECT_EQ(delta.value_write_calls, 3U);
    EXPECT_EQ(delta.value_write_us, 280000U);
    EXPECT_EQ(delta.value_remove_calls, 3U);
    EXPECT_EQ(delta.value_remove_us, 200000U);
    EXPECT_EQ(delta.value_read_us + delta.value_write_us + delta.value_remove_us, delta.value_us);
    EXPECT_EQ(delta.append_us + delta.value_us, delta.io_us);
    EXPECT_EQ(delta.lfs_size_calls, 1U);
    EXPECT_EQ(delta.lfs_size_us, 49000U);
    EXPECT_LE(delta.lfs_size_us, delta.space_us);
}

// C-SDK-009: fixed history/input/reply end-to-end turn on ESP deployment config (background path).
TEST(TurnDiagnostics, FixedTextTurnBackgroundPathCorrelatesUniqueBoundariesAndSumsExactly) {
    const auto sample = run_fixed_text_turn(ayni::strategies::Persistence::background);
    ASSERT_TRUE(sample.summary.complete);
    EXPECT_EQ(sample.summary.order, Order::async);
    EXPECT_EQ(sample.summary.turn, 2U);
    EXPECT_NE(sample.summary.goal, 0U);
    EXPECT_NE(sample.summary.task, 0U);
    EXPECT_NE(sample.call_id, 0U);
    EXPECT_EQ(sample.stage_callbacks, 5U);
    EXPECT_EQ(sample.confirm_busy_hits, 2U);
    EXPECT_EQ(sample.collect_busy_hits, 3U);
    EXPECT_EQ(sample.summary.total_us, sample.answer_saved_us - sample.model_posted_us);
    EXPECT_EQ(std::accumulate(sample.summary.segments_us.begin(), sample.summary.segments_us.end(),
                              uint64_t{0}),
              sample.summary.total_us);
    EXPECT_GT(sample.storage.append_calls, 0U);
    EXPECT_GT(sample.storage.value_write_calls, 0U);
    EXPECT_GT(sample.storage.value_remove_calls, 0U);
    std::printf(
        "[SimHost sample async] turn=%llu call=%llu goal=%llu task=%llu "
        "model_posted_us=%llu answer_saved_us=%llu total_us=%llu "
        "core_us=%llu publish_us=%llu save_us=%llu collect_us=%llu ack_us=%llu answer_us=%llu "
        "append=%llu/%lluus vread=%llu/%lluus vwrite=%llu/%lluus vremove=%llu/%lluus "
        "lfs_size=%llu/%lluus queue_us=%llu io_us=%llu\n",
        static_cast<unsigned long long>(sample.summary.turn),
        static_cast<unsigned long long>(sample.call_id),
        static_cast<unsigned long long>(sample.summary.goal),
        static_cast<unsigned long long>(sample.summary.task),
        static_cast<unsigned long long>(sample.model_posted_us),
        static_cast<unsigned long long>(sample.answer_saved_us),
        static_cast<unsigned long long>(sample.summary.total_us),
        static_cast<unsigned long long>(sample.summary.segments_us[0]),
        static_cast<unsigned long long>(sample.summary.segments_us[1]),
        static_cast<unsigned long long>(sample.summary.segments_us[2]),
        static_cast<unsigned long long>(sample.summary.segments_us[3]),
        static_cast<unsigned long long>(sample.summary.segments_us[4]),
        static_cast<unsigned long long>(sample.summary.segments_us[5]),
        static_cast<unsigned long long>(sample.storage.append_calls),
        static_cast<unsigned long long>(sample.storage.append_us),
        static_cast<unsigned long long>(sample.storage.value_read_calls),
        static_cast<unsigned long long>(sample.storage.value_read_us),
        static_cast<unsigned long long>(sample.storage.value_write_calls),
        static_cast<unsigned long long>(sample.storage.value_write_us),
        static_cast<unsigned long long>(sample.storage.value_remove_calls),
        static_cast<unsigned long long>(sample.storage.value_remove_us),
        static_cast<unsigned long long>(sample.storage.lfs_size_calls),
        static_cast<unsigned long long>(sample.storage.lfs_size_us),
        static_cast<unsigned long long>(sample.storage.queue_us),
        static_cast<unsigned long long>(sample.storage.io_us));
}

// C-SDK-009: same fixed turn on direct durable path preserves publish_durable -> checkpoint order.
TEST(TurnDiagnostics, FixedTextTurnDirectPathPreservesOrderAndSumsExactly) {
    const auto sample = run_fixed_text_turn(ayni::strategies::Persistence::durable);
    ASSERT_TRUE(sample.summary.complete);
    EXPECT_EQ(sample.summary.order, Order::direct);
    EXPECT_EQ(sample.summary.turn, 2U);
    EXPECT_NE(sample.summary.goal, 0U);
    EXPECT_NE(sample.summary.task, 0U);
    EXPECT_EQ(sample.stage_callbacks, 5U);
    EXPECT_EQ(sample.confirm_busy_hits, 2U);
    EXPECT_EQ(sample.collect_busy_hits, 3U);
    EXPECT_EQ(sample.summary.total_us, sample.answer_saved_us - sample.model_posted_us);
    EXPECT_EQ(std::accumulate(sample.summary.segments_us.begin(), sample.summary.segments_us.end(),
                              uint64_t{0}),
              sample.summary.total_us);
    std::printf("[SimHost sample direct] turn=%llu call=%llu goal=%llu task=%llu "
                "model_posted_us=%llu answer_saved_us=%llu total_us=%llu "
                "publish_us=%llu checkpoint_us=%llu save_us=%llu collect_us=%llu ack_us=%llu "
                "answer_us=%llu\n",
                static_cast<unsigned long long>(sample.summary.turn),
                static_cast<unsigned long long>(sample.call_id),
                static_cast<unsigned long long>(sample.summary.goal),
                static_cast<unsigned long long>(sample.summary.task),
                static_cast<unsigned long long>(sample.model_posted_us),
                static_cast<unsigned long long>(sample.answer_saved_us),
                static_cast<unsigned long long>(sample.summary.total_us),
                static_cast<unsigned long long>(sample.summary.segments_us[0]),
                static_cast<unsigned long long>(sample.summary.segments_us[1]),
                static_cast<unsigned long long>(sample.summary.segments_us[2]),
                static_cast<unsigned long long>(sample.summary.segments_us[3]),
                static_cast<unsigned long long>(sample.summary.segments_us[4]),
                static_cast<unsigned long long>(sample.summary.segments_us[5]));
}

} // namespace
