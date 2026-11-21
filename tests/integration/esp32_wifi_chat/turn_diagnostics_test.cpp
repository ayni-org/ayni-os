// Diagnostic breakdown checks for ESP32 Wi-Fi Chat publication, model handoff and storage metrics.
#include "configuration.h"
#include "diagnostic_log.h"
#include "turn_diagnostics.h"
#include <algorithm>
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
#include <optional>
#include <string>
#include <vector>

namespace {
using ayni::initialized;
using ayni::hosts::esp_idf::StorageMetrics;
using ayni::hosts::storage::FileValueRequest;
using ayni::hosts::storage::FileValueResult;
using Boundary = wifichat::PublicationTimeline::Boundary;
using Stage = wifichat::PublicationTimeline::Stage;
using Order = wifichat::PublicationTimeline::Order;
using Overlap = wifichat::UiFrameWindow::Overlap;

ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}

// Asynchronous in-memory FileValue worker mirroring Host::Storage::serve():
// publishes completed service/file counters BEFORE invoking the completion callback, then post_us.
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
        // Simulate periodic lfs_fs_size() reconciliation every 4 writes (like SpaceLedger).
        if (writes_since_measure_ >= 4U) {
            constexpr uint64_t measure_us = 25000;
            writes_since_measure_ = 0;
            host_.advance_us(measure_us);
            metrics_.space_us += measure_us;
            ++metrics_.files.space_measure_calls;
            metrics_.files.space_measure_us += measure_us;
        }
        const auto before_post = host_.now_us();
        metrics_.record_value(r, result, service_us, before_post - item.submitted_us);
        r.complete(r.context, result);
        metrics_.post_us += host_.now_us() - before_post;
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

struct EndToEndOptions {
    ayni::strategies::Persistence persistence = ayni::strategies::Persistence::background;
    uint32_t confirm_busy_count = 2;
    uint32_t collect_busy_count = 3;
    uint64_t confirm_delay_us = 0;
    uint64_t collect_delay_us = 0;
    uint64_t log_emit_delay_us = 0; // Simulated synchronous stdout delay per emitted line.
};

struct EndToEndSample {
    wifichat::PublicationTimeline timeline;
    wifichat::PublicationTimeline::Summary summary;
    uint64_t call_id = 0;
    uint64_t model_posted_us = 0, answer_saved_us = 0;
    uint64_t first_log_emit_us = 0;
    uint32_t stage_callbacks = 0;
    uint32_t confirm_busy_hits = 0, collect_busy_hits = 0;
    std::optional<Stage> confirm_hit_stage, collect_hit_stage;
    std::vector<std::string> emitted_lines;
};

void expect_additive_summary(const wifichat::PublicationTimeline::Summary &summary) {
    ASSERT_TRUE(summary.complete);
    ASSERT_EQ(summary.segment_count, wifichat::PublicationTimeline::max_segments);
    uint64_t sum_wall = 0, sum_queue = 0, sum_io = 0, sum_space = 0;
    uint64_t sum_append_n = 0, sum_append_us = 0;
    uint64_t sum_vread_n = 0, sum_vread_us = 0;
    uint64_t sum_vwrite_n = 0, sum_vwrite_us = 0;
    uint64_t sum_vremove_n = 0, sum_vremove_us = 0;
    uint64_t sum_lfs_n = 0, sum_lfs_us = 0;
    for (size_t i = 0; i < summary.segment_count; ++i) {
        const auto &d = summary.segments[i].io;
        sum_wall += d.wall_us;
        sum_queue += d.queue_us;
        sum_io += d.io_us;
        sum_space += d.space_us;
        sum_append_n += d.append_calls;
        sum_append_us += d.append_us;
        sum_vread_n += d.value_read_calls;
        sum_vread_us += d.value_read_us;
        sum_vwrite_n += d.value_write_calls;
        sum_vwrite_us += d.value_write_us;
        sum_vremove_n += d.value_remove_calls;
        sum_vremove_us += d.value_remove_us;
        sum_lfs_n += d.lfs_size_calls;
        sum_lfs_us += d.lfs_size_us;
    }
    EXPECT_EQ(sum_wall, summary.total.wall_us);
    EXPECT_EQ(sum_wall, summary.end_us - summary.start_us);
    EXPECT_EQ(sum_queue, summary.total.queue_us);
    EXPECT_EQ(sum_io, summary.total.io_us);
    EXPECT_EQ(sum_space, summary.total.space_us);
    EXPECT_EQ(sum_append_n, summary.total.append_calls);
    EXPECT_EQ(sum_append_us, summary.total.append_us);
    EXPECT_EQ(sum_vread_n, summary.total.value_read_calls);
    EXPECT_EQ(sum_vread_us, summary.total.value_read_us);
    EXPECT_EQ(sum_vwrite_n, summary.total.value_write_calls);
    EXPECT_EQ(sum_vwrite_us, summary.total.value_write_us);
    EXPECT_EQ(sum_vremove_n, summary.total.value_remove_calls);
    EXPECT_EQ(sum_vremove_us, summary.total.value_remove_us);
    EXPECT_EQ(sum_lfs_n, summary.total.lfs_size_calls);
    EXPECT_EQ(sum_lfs_us, summary.total.lfs_size_us);
}

EndToEndSample run_fixed_text_turn(const EndToEndOptions &opts) {
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
    loop_cfg.persistence = opts.persistence;
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

    auto combined_metrics = [&]() -> StorageMetrics {
        auto m = values.metrics();
        m.completed += journal_metrics.completed;
        m.append_calls += journal_metrics.append_calls;
        m.append_us += journal_metrics.append_us;
        m.io_us += journal_metrics.io_us;
        return m;
    };

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
            // Publish completed journal append counters BEFORE notifying Core/owner.
            journal_metrics.completed += pending;
            journal_metrics.append_calls += pending;
            journal_metrics.append_us += append_us * pending;
            journal_metrics.io_us += append_us * pending;
            host.complete_storage();
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
        ayni::hosts::sim::Host *host = nullptr;
        const ayni_body_store_v1 *inner_store = nullptr;
        ayni::runtime::BodyCollection inner_col{};
        ayni_body_store_v1 store{};
        std::optional<Stage> current_stage;
        uint32_t confirm_busy_remaining = 0, confirm_busy_hits = 0;
        uint32_t collect_busy_remaining = 0, collect_busy_hits = 0;
        uint64_t confirm_delay_us = 0, collect_delay_us = 0;
        std::optional<Stage> confirm_hit_stage, collect_hit_stage;
    } retry;
    retry.host = &host;
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
            r.confirm_hit_stage = r.current_stage;
            if (r.confirm_busy_hits == 1U && r.confirm_delay_us) {
                r.host->advance_us(r.confirm_delay_us);
            }
            return AYNI_BUSY;
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
            r.collect_hit_stage = r.current_stage;
            if (r.collect_busy_hits == 1U && r.collect_delay_us) {
                r.host->advance_us(r.collect_delay_us);
            }
            return AYNI_BUSY;
        }
        return r.inner_col.collected(r.inner_col.context, sc);
    };

    struct ObserverCtx {
        ayni::hosts::sim::Host *host = nullptr;
        decltype(combined_metrics) *snapshot = nullptr;
        RetryWrapper *retry = nullptr;
        const EndToEndOptions *opts = nullptr;
        wifichat::PublicationTimeline timeline;
        bool armed = false;
        uint32_t callbacks = 0;
    } obs{&host, &combined_metrics, &retry, &opts, {}, false, 0};

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
    scfg.persistence = opts.persistence;
    scfg.observer = {&obs, [](void *c, uint64_t g, uint64_t t, Stage st) {
                         auto &o = *static_cast<ObserverCtx *>(c);
                         if (!o.armed) {
                             return;
                         }
                         ++o.callbacks;
                         o.retry->current_stage = st;
                         // Arm backpressure by stage so background draft confirmation is untouched.
                         if (st == Stage::checkpoint_ready) {
                             o.retry->confirm_busy_remaining = o.opts->confirm_busy_count;
                             o.retry->confirm_delay_us = o.opts->confirm_delay_us;
                         } else if (st == Stage::record_saved) {
                             o.retry->collect_busy_remaining = o.opts->collect_busy_count;
                             o.retry->collect_delay_us = o.opts->collect_delay_us;
                         }
                         EXPECT_TRUE(
                             o.timeline.record_stage(g, t, st, o.host->now_us(), (*o.snapshot)()));
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

    // Turn 2: fixed text input & reply; sampling only inside model_result_posted -> answer_saved.
    model.reply = "固定最终诊断回复正文，用于核对发布分段耗时。";
    obs.timeline.reset(2);
    obs.armed = true;
    const uint64_t prev_call = model.last_posted_call;
    EXPECT_EQ(session.begin(text("固定文本输入")), AYNI_OK);
    EXPECT_TRUE(until([&] { return model.last_posted_call != prev_call; }));
    const uint64_t model_posted_us = model.last_posted_us;
    obs.timeline.mark_model_posted(model_posted_us, combined_metrics());

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
    EXPECT_TRUE(obs.timeline.record_answer_saved(answer_saved_us, combined_metrics()));

    // Deferred emission strictly AFTER answer_saved sampling (simulating presentation handoff).
    struct LogSink {
        ayni::hosts::sim::Host *host = nullptr;
        uint64_t emit_delay_us = 0;
        uint64_t first_emit_us = 0;
        std::vector<std::string> lines;
    } sink{&host, opts.log_emit_delay_us, 0, {}};
    obs.timeline.emit_report(&sink, [](void *ctx, const char *line, size_t size) {
        auto &s = *static_cast<LogSink *>(ctx);
        if (!s.first_emit_us) {
            s.first_emit_us = s.host->now_us();
        }
        if (s.emit_delay_us) {
            s.host->advance_us(s.emit_delay_us);
        }
        s.lines.emplace_back(line, size);
    });

    EndToEndSample out{};
    out.timeline = obs.timeline;
    out.summary = obs.timeline.summary();
    out.call_id = model.last_posted_call;
    out.model_posted_us = model_posted_us;
    out.answer_saved_us = answer_saved_us;
    out.first_log_emit_us = sink.first_emit_us;
    out.stage_callbacks = obs.callbacks;
    out.confirm_busy_hits = retry.confirm_busy_hits;
    out.collect_busy_hits = retry.collect_busy_hits;
    out.confirm_hit_stage = retry.confirm_hit_stage;
    out.collect_hit_stage = retry.collect_hit_stage;
    out.emitted_lines = std::move(sink.lines);

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

// C-HVAL-009: chunk/manifest I/O remains identifiable when one payload has two owners.
TEST(TurnDiagnostics, ValueRolesAccountForShortMultichunkAndSharedBody) {
    using ayni::hosts::storage::FileValueRole;
    const auto index = [](FileValueRole role) { return static_cast<size_t>(role); };
    ayni::hosts::sim::Host host(1000);
    QueuedValueStore values(host);
    ayni::runtime::FileBodies bodies;
    ASSERT_EQ(bodies.configure(values.port(), 32U * 1024U), AYNI_OK);
    const auto *port = bodies.store();
    const auto drain = [&] {
        for (size_t i = 0; i < 64 && !bodies.idle(); ++i) {
            ayni::runtime::FileBodies::progress(&bodies, 0);
            host.advance_us(1000); // Accepted requests wait for a worker opportunity.
            values.advance();
        }
        EXPECT_TRUE(bodies.idle());
    };
    const auto persist = [&](uint64_t scope, const std::string &value, ayni_body_ref_v1 &ref) {
        EXPECT_EQ(port->stage(port->context, scope,
                              {reinterpret_cast<const uint8_t *>(value.data()),
                               static_cast<uint32_t>(value.size())},
                              &ref),
                  AYNI_OK);
        EXPECT_EQ(port->confirm(port->context, &ref), AYNI_NOT_READY);
        drain();
        EXPECT_EQ(port->confirm(port->context, &ref), AYNI_OK);
    };
    ayni_body_ref_v1 small{}, large{}, shared{};
    persist(1, "short", small);
    constexpr uint32_t cross_chunk_bytes = 4097;
    persist(2, std::string(cross_chunk_bytes, 'x'), large);
    ASSERT_EQ(port->share(port->context, 3, &small, &shared), AYNI_OK);
    EXPECT_EQ(port->confirm(port->context, &shared), AYNI_NOT_READY);
    drain();
    const auto chunk = index(FileValueRole::body_chunk);
    const auto manifest = index(FileValueRole::owner_manifest);
    const auto before = values.metrics();
    EXPECT_EQ(before.values[chunk].write.calls, 3U);
    EXPECT_EQ(before.values[chunk].write.bytes, 5U + cross_chunk_bytes);
    EXPECT_EQ(before.values[manifest].write.calls, 3U);
    EXPECT_GT(before.values[manifest].write.completion_us,
              before.values[manifest].write.service_us);
    port->release(port->context, &small);
    drain();
    EXPECT_EQ(values.metrics().values[chunk].remove.calls, 0U);
    EXPECT_EQ(values.metrics().values[manifest].write.calls, 4U);
    port->release(port->context, &shared);
    port->release(port->context, &large);
    drain();
    const auto cleanup = wifichat::StoragePhaseDelta::between(before, values.metrics(), 0);
    EXPECT_EQ(cleanup.values[chunk].remove.calls, 3U);
    EXPECT_EQ(cleanup.values[chunk].remove.bytes, 5U + cross_chunk_bytes);
    EXPECT_EQ(cleanup.values[manifest].remove.calls, 2U);
    EXPECT_GT(cleanup.values[manifest].remove.bytes, 0U);
    EXPECT_EQ(cleanup.value_remove_calls, 5U);
}

TEST(TurnDiagnostics, PublicationTimelineAsyncOrderRecordsEachBoundaryOnceAndSumsToTotal) {
    wifichat::PublicationTimeline timeline;
    timeline.reset(77);

    StorageMetrics m{};
    // Multi-round turn: earlier tool round is overwritten by the final model_result_posted.
    timeline.mark_model_posted(100000, m);
    m.append_calls = 1;
    m.append_us = 10000;
    m.io_us = 10000;
    timeline.mark_model_posted(200000, m);

    m.append_calls += 1;
    m.append_us += 9000;
    m.io_us += 9000;
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::checkpoint_ready, 212000, m));
    // Duplicate/retry callbacks do not overwrite or double-emit a boundary.
    EXPECT_FALSE(timeline.record_stage(11, 22, Stage::checkpoint_ready, 215000, m));
    // A late model_posted mark cannot disturb an in-progress publication.
    timeline.mark_model_posted(999999, m);

    m.append_calls += 1;
    m.append_us += 15000;
    m.value_write_calls += 2;
    m.value_write_us += 40000;
    m.io_us += 55000;
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::publish_durable, 290000, m));

    m.value_write_calls += 1;
    m.value_write_us += 120000;
    m.io_us += 120000;
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::record_saved, 450000, m));

    m.value_remove_calls += 4;
    m.value_remove_us += 580000;
    m.files.space_measure_calls += 1;
    m.files.space_measure_us += 30000;
    m.io_us += 580000;
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::collected, 1100000, m));

    m.append_calls += 1;
    m.append_us += 110000;
    m.io_us += 110000;
    ASSERT_TRUE(timeline.record_stage(11, 22, Stage::context_ready, 1240000, m));

    m.value_read_calls += 1;
    m.value_read_us += 4000;
    m.io_us += 4000;
    ASSERT_TRUE(timeline.record_answer_saved(1245000, m));
    EXPECT_FALSE(timeline.record_answer_saved(1300000, m));

    const auto summary = timeline.summary();
    expect_additive_summary(summary);
    EXPECT_EQ(summary.order, Order::async);
    EXPECT_EQ(summary.turn, 77U);
    EXPECT_EQ(summary.goal, 11U);
    EXPECT_EQ(summary.task, 22U);
    EXPECT_EQ(summary.total.wall_us, 1045000U);
    EXPECT_EQ(summary.longest_index, 3U); // record_saved -> collected (650 ms).
    EXPECT_EQ(summary.segments[0].from, Boundary::model_result_posted);
    EXPECT_EQ(summary.segments[0].to, Boundary::checkpoint_ready);
    EXPECT_EQ(summary.segments[0].io.wall_us, 12000U);
    EXPECT_EQ(summary.segments[1].from, Boundary::checkpoint_ready);
    EXPECT_EQ(summary.segments[1].to, Boundary::publish_durable);
    EXPECT_EQ(summary.segments[1].io.wall_us, 78000U);
    EXPECT_EQ(summary.segments[2].from, Boundary::publish_durable);
    EXPECT_EQ(summary.segments[2].to, Boundary::record_saved);
    EXPECT_EQ(summary.segments[2].io.wall_us, 160000U);
    EXPECT_EQ(summary.segments[2].io.value_write_calls, 1U);
    EXPECT_EQ(summary.segments[3].from, Boundary::record_saved);
    EXPECT_EQ(summary.segments[3].to, Boundary::collected);
    EXPECT_EQ(summary.segments[3].io.wall_us, 650000U);
    EXPECT_EQ(summary.segments[3].io.value_remove_calls, 4U);
    EXPECT_EQ(summary.segments[3].io.lfs_size_calls, 1U);
    EXPECT_EQ(summary.segments[4].from, Boundary::collected);
    EXPECT_EQ(summary.segments[4].to, Boundary::context_ready);
    EXPECT_EQ(summary.segments[4].io.wall_us, 140000U);
    EXPECT_EQ(summary.segments[5].from, Boundary::context_ready);
    EXPECT_EQ(summary.segments[5].to, Boundary::answer_saved);
    EXPECT_EQ(summary.segments[5].io.wall_us, 5000U);
    EXPECT_EQ(summary.segments[5].io.value_read_calls, 1U);
}

TEST(TurnDiagnostics, PublicationTimelineDirectOrderPreservesPublishBeforeCheckpoint) {
    wifichat::PublicationTimeline timeline;
    timeline.reset(88);
    StorageMetrics m{};
    timeline.mark_model_posted(500000, m);

    m.append_calls += 1;
    m.append_us += 45000;
    m.io_us += 45000;
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::publish_durable, 560000, m));
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::checkpoint_ready, 565000, m));
    m.value_write_calls += 1;
    m.value_write_us += 140000;
    m.io_us += 140000;
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::record_saved, 715000, m));
    m.value_remove_calls += 2;
    m.value_remove_us += 180000;
    m.io_us += 180000;
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::collected, 915000, m));
    m.append_calls += 1;
    m.append_us += 50000;
    m.io_us += 50000;
    ASSERT_TRUE(timeline.record_stage(31, 42, Stage::context_ready, 980000, m));
    ASSERT_TRUE(timeline.record_answer_saved(984000, m));

    const auto summary = timeline.summary();
    expect_additive_summary(summary);
    EXPECT_EQ(summary.order, Order::direct);
    EXPECT_EQ(summary.total.wall_us, 484000U);
    EXPECT_EQ(summary.segments[0].from, Boundary::model_result_posted);
    EXPECT_EQ(summary.segments[0].to, Boundary::publish_durable);
    EXPECT_EQ(summary.segments[0].io.wall_us, 60000U);
    EXPECT_EQ(summary.segments[1].from, Boundary::publish_durable);
    EXPECT_EQ(summary.segments[1].to, Boundary::checkpoint_ready);
    EXPECT_EQ(summary.segments[1].io.wall_us, 5000U);
    EXPECT_EQ(summary.segments[2].from, Boundary::checkpoint_ready);
    EXPECT_EQ(summary.segments[2].to, Boundary::record_saved);
    EXPECT_EQ(summary.segments[2].io.wall_us, 150000U);
    EXPECT_EQ(summary.segments[3].io.wall_us, 200000U);
    EXPECT_EQ(summary.segments[4].io.wall_us, 65000U);
    EXPECT_EQ(summary.segments[5].io.wall_us, 4000U);
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

// C-SDK-009: fixed history/input/reply end-to-end turn on ESP background config.
// Also verifies stage-armed backpressure and known-wait attribution to the exact owning segment.
TEST(TurnDiagnostics, FixedTextTurnBackgroundPathCorrelatesUniqueBoundariesAndSumsExactly) {
    EndToEndOptions base_opts{};
    base_opts.persistence = ayni::strategies::Persistence::background;
    const auto base = run_fixed_text_turn(base_opts);
    expect_additive_summary(base.summary);
    using RoleMetrics = ayni::hosts::esp_idf::ValueRoleMetrics;
    using Operation = ayni::hosts::esp_idf::ValueOperationMetrics;
    for (const auto member : {&RoleMetrics::read, &RoleMetrics::write, &RoleMetrics::remove}) {
        for (size_t role = 0; role < base.summary.total.values.size(); ++role) {
            Operation sum{};
            for (size_t i = 0; i < base.summary.segment_count; ++i) {
                const auto &part = base.summary.segments[i].io.values[role].*member;
                sum.calls += part.calls;
                sum.bytes += part.bytes;
                sum.service_us += part.service_us;
                sum.completion_us += part.completion_us;
            }
            const auto &total = base.summary.total.values[role].*member;
            EXPECT_EQ(sum.calls, total.calls);
            EXPECT_EQ(sum.bytes, total.bytes);
            EXPECT_EQ(sum.service_us, total.service_us);
            EXPECT_EQ(sum.completion_us, total.completion_us);
        }
    }
    EXPECT_EQ(base.summary.order, Order::async);
    EXPECT_EQ(base.summary.turn, 2U);
    EXPECT_NE(base.summary.goal, 0U);
    EXPECT_NE(base.summary.task, 0U);
    EXPECT_NE(base.call_id, 0U);
    EXPECT_EQ(base.stage_callbacks, 5U);
    EXPECT_EQ(base.confirm_busy_hits, 2U);
    EXPECT_EQ(base.collect_busy_hits, 3U);
    ASSERT_TRUE(base.confirm_hit_stage.has_value());
    EXPECT_EQ(*base.confirm_hit_stage, Stage::checkpoint_ready);
    ASSERT_TRUE(base.collect_hit_stage.has_value());
    EXPECT_EQ(*base.collect_hit_stage, Stage::record_saved);
    EXPECT_GE(base.summary.segments[2].io.value_write_calls, 1U); // publish_durable -> record_saved
    EXPECT_GE(base.summary.segments[3].io.value_remove_calls, 1U); // record_saved -> collected
    EXPECT_EQ(std::count_if(base.emitted_lines.begin(), base.emitted_lines.end(),
                            [](const auto &line) { return line.rfind("wifi pub ", 0) == 0; }),
              6);
    EXPECT_NE(std::find_if(base.emitted_lines.begin(), base.emitted_lines.end(),
                           [](const auto &line) {
                               return line.find("role=conversation_root") != std::string::npos;
                           }),
              base.emitted_lines.end());

    // Known-wait attribution: inject +70 ms in confirm (seg 1) and +110 ms in collect (seg 3).
    EndToEndOptions delayed_opts = base_opts;
    delayed_opts.confirm_delay_us = 70000;
    delayed_opts.collect_delay_us = 110000;
    const auto delayed = run_fixed_text_turn(delayed_opts);
    expect_additive_summary(delayed.summary);
    for (size_t i = 0; i < base.summary.segment_count; ++i) {
        const uint64_t expected_extra = (i == 1U ? delayed_opts.confirm_delay_us : 0U) +
                                        (i == 3U ? delayed_opts.collect_delay_us : 0U);
        EXPECT_EQ(delayed.summary.segments[i].io.wall_us,
                  base.summary.segments[i].io.wall_us + expected_extra)
            << "segment=" << i;
    }

    for (const auto &line : base.emitted_lines) {
        std::printf("[SimHost async] %s", line.c_str());
    }
}

// C-SDK-009: same fixed turn on direct durable path preserves publish_durable -> checkpoint order
// and attributes known confirm/collect waits strictly to segments 2 and 3.
TEST(TurnDiagnostics, FixedTextTurnDirectPathPreservesOrderAndSumsExactly) {
    EndToEndOptions base_opts{};
    base_opts.persistence = ayni::strategies::Persistence::durable;
    const auto base = run_fixed_text_turn(base_opts);
    expect_additive_summary(base.summary);
    EXPECT_EQ(base.summary.order, Order::direct);
    EXPECT_EQ(base.summary.turn, 2U);
    EXPECT_NE(base.summary.goal, 0U);
    EXPECT_NE(base.summary.task, 0U);
    EXPECT_EQ(base.stage_callbacks, 5U);
    EXPECT_EQ(base.confirm_busy_hits, 2U);
    EXPECT_EQ(base.collect_busy_hits, 3U);
    ASSERT_TRUE(base.confirm_hit_stage.has_value());
    EXPECT_EQ(*base.confirm_hit_stage, Stage::checkpoint_ready);
    ASSERT_TRUE(base.collect_hit_stage.has_value());
    EXPECT_EQ(*base.collect_hit_stage, Stage::record_saved);
    EXPECT_EQ(base.summary.segments[0].to, Boundary::publish_durable);
    EXPECT_EQ(base.summary.segments[1].from, Boundary::publish_durable);
    EXPECT_EQ(base.summary.segments[1].to, Boundary::checkpoint_ready);

    EndToEndOptions delayed_opts = base_opts;
    delayed_opts.confirm_delay_us = 70000;
    delayed_opts.collect_delay_us = 110000;
    const auto delayed = run_fixed_text_turn(delayed_opts);
    expect_additive_summary(delayed.summary);
    for (size_t i = 0; i < base.summary.segment_count; ++i) {
        const uint64_t expected_extra = (i == 2U ? delayed_opts.confirm_delay_us : 0U) +
                                        (i == 3U ? delayed_opts.collect_delay_us : 0U);
        EXPECT_EQ(delayed.summary.segments[i].io.wall_us,
                  base.summary.segments[i].io.wall_us + expected_extra)
            << "segment=" << i;
    }

    for (const auto &line : base.emitted_lines) {
        std::printf("[SimHost direct] %s", line.c_str());
    }
}

// Worker completion-visibility contract: service counters are published BEFORE completion notifies
// owner, so an owner sampling immediately on completion observes the triggering write in
// record_saved and the triggering remove in collected (one each), with exact head-to-tail sums.
TEST(TurnDiagnostics, CompletionOrderingAttributesTriggeringWriteAndRemoveToClosingSegments) {
    ayni::hosts::sim::Host host(2000000);
    QueuedValueStore store(host);
    wifichat::PublicationTimeline timeline;
    timeline.reset(9);

    constexpr uint64_t owner_id = 1, root_obj = 100, old_body_obj = 200;
    // Seed an existing object so the later remove erases a real value.
    const uint8_t seed_bytes[4] = {1, 2, 3, 4};
    FileValueRequest seed_req{};
    seed_req.kind = FileValueRequest::Kind::write;
    seed_req.key = {owner_id, old_body_obj};
    seed_req.generation = 1;
    seed_req.data = seed_bytes;
    seed_req.size = sizeof(seed_bytes);
    ayni_status_t seed_status = AYNI_INTERNAL;
    seed_req.context = &seed_status;
    seed_req.complete = [](void *ctx, const FileValueResult &res) noexcept {
        *static_cast<ayni_status_t *>(ctx) = res.status;
    };
    ASSERT_EQ(store.port().submit(store.port().context, &seed_req), AYNI_SUBMIT_ACCEPTED);
    store.advance();
    ASSERT_EQ(seed_status, AYNI_OK);

    timeline.mark_model_posted(host.now_us(), store.metrics());
    host.advance_us(10000);
    ASSERT_TRUE(
        timeline.record_stage(50, 60, Stage::checkpoint_ready, host.now_us(), store.metrics()));
    host.advance_us(15000);
    ASSERT_TRUE(
        timeline.record_stage(50, 60, Stage::publish_durable, host.now_us(), store.metrics()));

    struct ImmediateSample {
        ayni::hosts::sim::Host *host;
        QueuedValueStore *store;
        wifichat::PublicationTimeline *timeline;
        Stage stage;
        ayni_status_t status = AYNI_INTERNAL;
        bool recorded = false;
    };

    // 1) One root-record write whose completion callback immediately samples record_saved.
    ImmediateSample on_write{&host, &store, &timeline, Stage::record_saved};
    const uint8_t root_bytes[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    FileValueRequest write_req{};
    write_req.kind = FileValueRequest::Kind::write;
    write_req.key = {owner_id, root_obj};
    write_req.generation = 2;
    write_req.data = root_bytes;
    write_req.size = sizeof(root_bytes);
    write_req.context = &on_write;
    write_req.complete = [](void *ctx, const FileValueResult &res) noexcept {
        auto &s = *static_cast<ImmediateSample *>(ctx);
        s.status = res.status;
        s.recorded =
            s.timeline->record_stage(50, 60, s.stage, s.host->now_us(), s.store->metrics());
    };
    ASSERT_EQ(store.port().submit(store.port().context, &write_req), AYNI_SUBMIT_ACCEPTED);
    host.advance_us(3000);
    store.advance();
    ASSERT_EQ(on_write.status, AYNI_OK);
    ASSERT_TRUE(on_write.recorded);

    // 2) One value remove whose completion callback immediately samples collected.
    ImmediateSample on_remove{&host, &store, &timeline, Stage::collected};
    FileValueRequest remove_req{};
    remove_req.kind = FileValueRequest::Kind::remove;
    remove_req.key = {owner_id, old_body_obj};
    remove_req.context = &on_remove;
    remove_req.complete = [](void *ctx, const FileValueResult &res) noexcept {
        auto &s = *static_cast<ImmediateSample *>(ctx);
        s.status = res.status;
        s.recorded =
            s.timeline->record_stage(50, 60, s.stage, s.host->now_us(), s.store->metrics());
    };
    ASSERT_EQ(store.port().submit(store.port().context, &remove_req), AYNI_SUBMIT_ACCEPTED);
    host.advance_us(4000);
    store.advance();
    ASSERT_EQ(on_remove.status, AYNI_OK);
    ASSERT_TRUE(on_remove.recorded);

    host.advance_us(5000);
    ASSERT_TRUE(
        timeline.record_stage(50, 60, Stage::context_ready, host.now_us(), store.metrics()));
    host.advance_us(2000);
    ASSERT_TRUE(timeline.record_answer_saved(host.now_us(), store.metrics()));

    const auto summary = timeline.summary();
    expect_additive_summary(summary);
    // Write landed inside publish_durable -> record_saved (segment 2), not leaked to collected.
    EXPECT_EQ(summary.segments[2].to, Boundary::record_saved);
    EXPECT_EQ(summary.segments[2].io.value_write_calls, 1U);
    EXPECT_EQ(summary.segments[2].io.value_write_us, 18000U);
    EXPECT_EQ(summary.segments[2].io.value_remove_calls, 0U);
    // Remove landed inside record_saved -> collected (segment 3), not leaked to context_ready.
    EXPECT_EQ(summary.segments[3].to, Boundary::collected);
    EXPECT_EQ(summary.segments[3].io.value_write_calls, 0U);
    EXPECT_EQ(summary.segments[3].io.value_remove_calls, 1U);
    EXPECT_EQ(summary.segments[3].io.value_remove_us, 12000U);
    // Post-collected segments saw neither leaked write nor leaked remove.
    EXPECT_EQ(summary.segments[4].io.value_write_calls, 0U);
    EXPECT_EQ(summary.segments[4].io.value_remove_calls, 0U);
    EXPECT_EQ(summary.segments[5].io.value_write_calls, 0U);
    EXPECT_EQ(summary.segments[5].io.value_remove_calls, 0U);
}

// Deferred logging isolation: adding heavy synchronous output delay (+25 ms/line) must not change
// any of the 7 samples, 6 segment durations, or I/O deltas, and first emission must follow
// answer_saved.
TEST(TurnDiagnostics, DeferredOutputProducesIdenticalSamplesUnderInstantAndSlowLogSinks) {
    EndToEndOptions fast_opts{};
    fast_opts.persistence = ayni::strategies::Persistence::background;
    fast_opts.log_emit_delay_us = 0;

    EndToEndOptions slow_opts = fast_opts;
    slow_opts.log_emit_delay_us = 25000;

    const auto fast = run_fixed_text_turn(fast_opts);
    const auto slow = run_fixed_text_turn(slow_opts);

    ASSERT_EQ(fast.timeline.sample_count(), wifichat::PublicationTimeline::max_samples);
    ASSERT_EQ(slow.timeline.sample_count(), wifichat::PublicationTimeline::max_samples);
    for (size_t i = 0; i < wifichat::PublicationTimeline::max_samples; ++i) {
        const auto &a = fast.timeline.sample(i);
        const auto &b = slow.timeline.sample(i);
        EXPECT_EQ(a.boundary, b.boundary) << "sample=" << i;
        EXPECT_EQ(a.at_us, b.at_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.completed, b.metrics.completed) << "sample=" << i;
        EXPECT_EQ(a.metrics.queue_us, b.metrics.queue_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.io_us, b.metrics.io_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.append_calls, b.metrics.append_calls) << "sample=" << i;
        EXPECT_EQ(a.metrics.append_us, b.metrics.append_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_read_calls, b.metrics.value_read_calls) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_read_us, b.metrics.value_read_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_write_calls, b.metrics.value_write_calls) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_write_us, b.metrics.value_write_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_remove_calls, b.metrics.value_remove_calls) << "sample=" << i;
        EXPECT_EQ(a.metrics.value_remove_us, b.metrics.value_remove_us) << "sample=" << i;
        EXPECT_EQ(a.metrics.files.space_measure_calls, b.metrics.files.space_measure_calls)
            << "sample=" << i;
        EXPECT_EQ(a.metrics.files.space_measure_us, b.metrics.files.space_measure_us)
            << "sample=" << i;
    }
    EXPECT_GE(fast.first_log_emit_us, fast.answer_saved_us);
    EXPECT_GE(slow.first_log_emit_us, slow.answer_saved_us);
    EXPECT_EQ(fast.emitted_lines, slow.emitted_lines);
    ASSERT_GT(slow.emitted_lines.size(), 7U);
    for (const auto &line : slow.emitted_lines) {
        EXPECT_NE(line.find("turn=2 "), std::string::npos) << line;
        EXPECT_LT(line.size(), wifichat::DiagnosticLog::line_bytes) << line;
    }
}

// Verify actual worst-case formatted lines fit within DiagnosticLog::line_bytes (384 bytes).
TEST(TurnDiagnostics, FormattedReportLinesFitWithinDiagnosticLogSlot) {
    wifichat::PublicationTimeline::Segment worst_seg{};
    worst_seg.from = Boundary::model_result_posted;
    worst_seg.to = Boundary::checkpoint_ready;
    worst_seg.io.wall_us = 9999999999ULL;
    worst_seg.io.queue_us = 9999999999ULL;
    worst_seg.io.io_us = 9999999999ULL;
    worst_seg.io.append_calls = 999999ULL;
    worst_seg.io.append_us = 9999999999ULL;
    worst_seg.io.value_read_calls = 999999ULL;
    worst_seg.io.value_read_us = 9999999999ULL;
    worst_seg.io.value_write_calls = 999999ULL;
    worst_seg.io.value_write_us = 9999999999ULL;
    worst_seg.io.value_remove_calls = 999999ULL;
    worst_seg.io.value_remove_us = 9999999999ULL;
    worst_seg.io.lfs_size_calls = 999999ULL;
    worst_seg.io.lfs_size_us = 9999999999ULL;

    char seg_line[wifichat::DiagnosticLog::line_bytes]{};
    const int seg_len = wifichat::PublicationTimeline::format_segment(
        99999999ULL, 5U, 6U, worst_seg, seg_line, sizeof(seg_line));
    EXPECT_GT(seg_len, 0);
    EXPECT_LT(static_cast<size_t>(seg_len), wifichat::DiagnosticLog::line_bytes) << seg_line;

    char sum_line[wifichat::DiagnosticLog::line_bytes]{};
    const int sum_len = wifichat::PublicationTimeline::format_summary(
        99999999ULL, UINT64_C(429496729600001), UINT64_C(429496729600002), Order::direct, true,
        worst_seg.io, worst_seg, sum_line, sizeof(sum_line));
    EXPECT_GT(sum_len, 0);
    EXPECT_LT(static_cast<size_t>(sum_len), wifichat::DiagnosticLog::line_bytes) << sum_line;
}

} // namespace
