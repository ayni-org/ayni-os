#include "body_cache.h"
#include "harness.h"
#include "protocol.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <ayni/strategies/builtin.h>
#include <ayni/values.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <vector>
namespace {
ayni_string_v1 text(std::string_view value) {
    return {value.data(), uint32_t(value.size())};
}
struct Bodies {
    std::map<uint64_t, std::string> values;
    static ayni_status_t write(void *context, uint64_t, ayni_bytes_v1 bytes,
                               ayni_body_ref_v1 *reference) {
        auto &self = *static_cast<Bodies *>(context);
        std::string value(reinterpret_cast<const char *>(bytes.data), bytes.size);
        for (const auto &entry : self.values) {
            if (entry.second == value) {
                *reference = {entry.first, ayni_body_digest(bytes), bytes.size, 0};
                return AYNI_OK;
            }
        }
        const auto id = self.values.size() + 1;
        self.values[id] = std::move(value);
        *reference = {id, ayni_body_digest(bytes), bytes.size, 0};
        return AYNI_OK;
    }
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *reference, uint8_t *out,
                              uint32_t capacity) {
        const auto &values = static_cast<Bodies *>(context)->values;
        const auto found = values.find(reference->id);
        if (found == values.end()) {
            return AYNI_NOT_FOUND;
        }
        if (capacity < found->second.size()) {
            return AYNI_BUFFER_TOO_SMALL;
        }
        std::memcpy(out, found->second.data(), found->second.size());
        return AYNI_OK;
    }
    static void release(void *, const ayni_body_ref_v1 *) {}
    ayni_body_store_v1 port() {
        return {sizeof(ayni_body_store_v1), 0, this, write, read, release};
    }
};
struct Serving {
    unsigned requests = 0;
    bool answered = false;
    std::string key;
    std::string response = "你好，这是直接回答。";
    size_t maximum_prompt = 0;
    ayni_policy_output_v1 *output = nullptr;
    static ayni_status_t request(void *context, const ayni_call_request_v1 *request) {
        auto &self = *static_cast<Serving *>(context);
        ++self.requests;
        self.key.assign(request->step_key.data, request->step_key.size);
        ayni::ValueReader reader;
        ayni_body_ref_v1 body{};
        if (reader.read(request->value) == AYNI_OK &&
            ayni::ValueReader::body(reader.field(AYNI_MODEL_BODY_FIELD_ID), body)) {
            self.maximum_prompt = std::max(self.maximum_prompt, size_t(body.size));
        }
        self.output->intent_count = 1;
        self.output->intents[0] = {};
        self.output->intents[0].struct_size = sizeof(ayni_policy_intent_v1);
        self.output->intents[0].kind = request->kind;
        return AYNI_OK;
    }
    static ayni_status_t result(const void *context, ayni_string_v1 key,
                                ayni_task_call_result_v1 *result) {
        const auto &self = *static_cast<const Serving *>(context);
        if (!self.answered || std::string_view(key.data, key.size) != self.key) {
            return AYNI_NOT_FOUND;
        }
        *result = {};
        result->struct_size = sizeof(*result);
        result->logical_call_id = 1;
        result->outcome = AYNI_CALL_COMPLETED;
        result->status = AYNI_OK;
        result->effect = AYNI_CALL_EFFECT_COMPLETE;
        result->text = text(self.response);
        return AYNI_OK;
    }
    static ayni_status_t visibility(const void *, const ayni_capability_context_v1 *,
                                    ayni_capability_context_v1 *out) {
        *out = {};
        out->boot_epoch = 1;
        out->owner_instance_id = 1;
        out->goal_type_version = 1;
        constexpr char id[] = "usb.chat";
        out->goal_type_size = sizeof(id) - 1;
        std::memcpy(out->goal_type_id, id, sizeof(id) - 1);
        return AYNI_OK;
    }
};
// Normal asynchronous-file facade: payloads represent flash, not the SDK's RAM cache.
struct ValueFiles {
    struct Value {
        uint64_t generation;
        std::vector<uint8_t> bytes;
    };
    std::map<std::pair<uint64_t, uint64_t>, Value> values;
    bool removal_capacity = true;
    static ayni_submit_result_t submit(void *context,
                                       const ayni::hosts::storage::FileValueRequest *r) {
        auto &self = *static_cast<ValueFiles *>(context);
        const auto key = std::make_pair(r->key.owner_instance_id, r->key.object_id);
        ayni::hosts::storage::FileValueResult result{AYNI_OK, 0, 0, 0};
        using Kind = ayni::hosts::storage::FileValueRequest::Kind;
        auto found = self.values.find(key);
        if (r->kind == Kind::remove) {
            if (!self.removal_capacity) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            self.values.erase(key);
        } else if (r->kind == Kind::write) {
            self.values[key] = {r->generation, {r->data, r->data + r->size}};
            result.generation = r->generation;
            result.size = r->size;
        } else if (found == self.values.end()) {
            result.status = AYNI_NOT_FOUND;
        } else if (r->size < found->second.bytes.size()) {
            result.status = AYNI_BUFFER_TOO_SMALL;
        } else {
            std::memcpy(r->buffer, found->second.bytes.data(), found->second.bytes.size());
            result.generation = found->second.generation;
            result.size = found->second.bytes.size();
        }
        r->complete(r->context, result);
        return AYNI_SUBMIT_ACCEPTED;
    }
};
void large_context(std::string_view glyph, unsigned rounds) {
    // Use the actual public ToolLoop AND FileBodies through the experiment cache.
    // Repeated complete normal turns grow its real history beyond the old 32KiB path.
    ValueFiles files;
    wifichat::BodyCache bodies;
    ASSERT_EQ(bodies.configure({&files, ValueFiles::submit}), AYNI_OK);
    const auto *store = bodies.store();
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> region((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    ayni_workspace_v1 workspace{sizeof(workspace), 0, region.data(),
                                region.size() * sizeof(uint64_t)};
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = text("usb.chat");
    config.model.kind = config.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = text("model.tools");
    config.summary.binding_id = text("summary.text");
    config.model.text = text("直接回答。");
    config.limits = {wifichat::harness::context_window_tokens, wifichat::harness::max_output_tokens,
                     wifichat::harness::compact_target_tokens,
                     wifichat::harness::tool_result_max_tokens};
    config.bodies = store;
    ayni::strategies::ToolLoop loop;
    ASSERT_EQ(loop.configure(config, &workspace), AYNI_OK);
    const auto inner = loop.describe();
    ayni::strategies::ConversationRecord record;
    record.id = 1;
    record.owner_instance_id = 1;
    std::string user, answer;
    while (user.size() + glyph.size() <= wifichat::input_bytes) {
        user += glyph;
    }
    while (answer.size() + glyph.size() <= wifichat::summary_bytes) {
        answer += glyph;
    }
    size_t maximum_prompt = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        ASSERT_EQ(bodies.reset_idle(), AYNI_OK);
        ayni::strategies::Encoded parameters;
        auto status = AYNI_NOT_READY;
        for (unsigned poll = 0; poll < 256 && status == AYNI_NOT_READY; ++poll) {
            status = ayni::strategies::Conversation::begin(&workspace, *store, record, text(user),
                                                           parameters);
            wifichat::BodyCache::ready(&bodies, 0);
        }
        ASSERT_EQ(status, AYNI_OK);
        Serving serving;
        serving.response = answer;
        ayni_task_snapshot_v1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.revision = 1;
        snapshot.task_id = round + 1;
        snapshot.goal_id = round + 1;
        snapshot.owner_instance_id = 1;
        snapshot.goal_type_version = 1;
        snapshot.goal_type_id = text("usb.chat");
        snapshot.goal_parameters = parameters.view();
        snapshot.step_capacity = AYNI_TASK_STEP_CAPACITY_MAX;
        snapshot.call_context = &serving;
        snapshot.call_result = Serving::result;
        snapshot.capability_context = Serving::visibility;
        ayni::strategies::Encoded committed, candidate;
        bool completed = false;
        for (unsigned poll = 0; poll < 256 && !completed; ++poll) {
            ayni_policy_intent_v1 intent{};
            ayni_policy_output_v1 output{};
            output.struct_size = sizeof(output);
            output.intent_capacity = 1;
            output.intents = &intent;
            output.checkpoint = candidate.bytes.data();
            output.checkpoint_capacity = candidate.bytes.size();
            output.request_call = Serving::request;
            output.call_context = &serving;
            serving.output = &output;
            ASSERT_EQ(inner.decide(inner.ctx, &snapshot, &output), AYNI_OK);
            wifichat::BodyCache::ready(&bodies, 0);
            if (output.disposition == AYNI_POLICY_OUTPUT_DEFER) {
                continue;
            }
            ASSERT_EQ(output.intent_count, 1U);
            candidate.size = output.checkpoint_size;
            committed = candidate;
            snapshot.checkpoint.state = committed.view();
            ++snapshot.revision;
            if (intent.kind == AYNI_POLICY_REQUEST_MODEL) {
                serving.answered = true;
            } else {
                ASSERT_EQ(intent.kind, AYNI_POLICY_REPORT_COMPLETION);
                completed = true;
            }
        }
        ASSERT_TRUE(completed);
        ASSERT_EQ(serving.requests, 1U); // No model confirmation, even for large history.
        maximum_prompt = std::max(maximum_prompt, serving.maximum_prompt);
        ayni::strategies::ConversationRecord next;
        ASSERT_EQ(
            ayni::strategies::Conversation::finish(&workspace, record, committed.view(), next),
            AYNI_OK);
        std::vector<ayni_body_ref_v1> before, keep;
        ayni::strategies::Conversation::references(record, before);
        ayni::strategies::Conversation::references(next, keep);
        for (const auto &ref : before) {
            if (std::none_of(keep.begin(), keep.end(),
                             [&](const auto &k) { return k.id == ref.id; })) {
                store->release(store->context, &ref);
            }
        }
        bodies.collect(record.id, keep);
        wifichat::BodyCache::ready(&bodies, 0);
        record = next;
    }
    EXPECT_GT(maximum_prompt, 400U * 1024U);
    EXPECT_LT(maximum_prompt, wifichat::harness::model_body_bytes);
    ASSERT_EQ(bodies.reset_idle(), AYNI_OK);
    std::vector<ayni::strategies::Round> retained;
    auto status = AYNI_NOT_READY;
    for (unsigned poll = 0; poll < 256 && status == AYNI_NOT_READY; ++poll) {
        status = ayni::strategies::Conversation::rounds(*store, record, retained);
        wifichat::BodyCache::ready(&bodies, 0);
    }
    ASSERT_EQ(status, AYNI_OK);
    ASSERT_FALSE(retained.empty());
    EXPECT_EQ(retained.back().answer, answer); // Cache replacement did not delete disk bodies.
}
} // namespace
TEST(WifiReply, LargeChineseContextUsesRealToolLoopAndColdFileBodies) {
    large_context("界", 39);
}
TEST(WifiReply, LargeFourByteContextUsesRealToolLoopAndColdFileBodies) {
    large_context("🌍", 52);
}
TEST(WifiReply, RealToolLoopCompletesTextAfterOneModelRequestAndSavesItsOwnHistoryFormat) {
    // User's single-answer flow: real Ayni ToolLoop and Conversation codecs, no replacement.
    Bodies bodies;
    const auto store = bodies.port();
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> region((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    ayni_workspace_v1 workspace{sizeof(workspace), 0, region.data(),
                                region.size() * sizeof(uint64_t)};
    ayni::strategies::ConversationRecord record;
    record.id = 1;
    record.owner_instance_id = 1;
    ayni::strategies::Encoded parameters;
    ASSERT_EQ(
        ayni::strategies::Conversation::begin(&workspace, store, record, text("你好"), parameters),
        AYNI_OK);
    ayni::strategies::ToolLoopConfig config;
    config.identity.id = text("usb.chat");
    config.model.kind = config.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    config.model.binding_id = text("model.tools");
    config.summary.binding_id = text("summary.text");
    config.model.text = text("你是Ayni，一个通用助手。");
    config.limits = {8192, 1800, 4096, 1024};
    config.bodies = &store;
    ayni::strategies::ToolLoop loop;
    ASSERT_EQ(loop.configure(config, &workspace), AYNI_OK);
    const auto inner = loop.describe();
    Serving service;
    ayni_task_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.revision = 1;
    snapshot.task_id = 1;
    snapshot.goal_id = 1;
    snapshot.owner_instance_id = 1;
    snapshot.goal_type_version = 1;
    snapshot.goal_type_id = text("usb.chat");
    snapshot.goal_parameters = parameters.view();
    snapshot.step_capacity = AYNI_TASK_STEP_CAPACITY_MAX;
    snapshot.call_context = &service;
    snapshot.call_result = Serving::result;
    snapshot.capability_context = Serving::visibility;
    ayni_policy_intent_v1 intent{};
    ayni::strategies::Encoded checkpoint;
    ayni_policy_output_v1 output{};
    output.struct_size = sizeof(output);
    output.intent_capacity = 1;
    output.intents = &intent;
    output.checkpoint = checkpoint.bytes.data();
    output.checkpoint_capacity = checkpoint.bytes.size();
    output.call_context = &service;
    output.request_call = Serving::request;
    service.output = &output;
    ASSERT_EQ(inner.decide(inner.ctx, &snapshot, &output), AYNI_OK);
    ASSERT_EQ(service.requests, 1U);
    EXPECT_EQ(service.key, "model.0");
    ASSERT_GT(output.checkpoint_size, 0U);
    checkpoint.size = output.checkpoint_size;
    snapshot.checkpoint.state = checkpoint.view();
    ++snapshot.revision;
    service.answered = true;
    output.intent_count = 0;
    ASSERT_EQ(inner.decide(inner.ctx, &snapshot, &output), AYNI_OK);
    EXPECT_EQ(service.requests, 1U);
    ASSERT_EQ(output.intent_count, 1U);
    EXPECT_EQ(intent.kind, AYNI_POLICY_REPORT_COMPLETION);
    checkpoint.size = output.checkpoint_size;
    ayni::strategies::ConversationRecord saved;
    ASSERT_EQ(ayni::strategies::Conversation::finish(&workspace, record, checkpoint.view(), saved),
              AYNI_OK);
    std::vector<ayni::strategies::Round> rounds;
    ASSERT_EQ(ayni::strategies::Conversation::rounds(store, saved, rounds), AYNI_OK);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].user, "你好");
    EXPECT_EQ(rounds[0].answer, "你好，这是直接回答。");
}
TEST(WifiReply, CacheResetRetainsCleanupUntilNormalFileQueueHasCapacity) {
    // ADR-0097: a normal full file queue cannot cause cache reset to discard deferred removals.
    ValueFiles files;
    wifichat::BodyCache bodies;
    ASSERT_EQ(bodies.configure({&files, ValueFiles::submit}), AYNI_OK);
    const auto *port = bodies.store();
    const uint8_t value[] = "retired body";
    ayni_body_ref_v1 reference{};
    auto written = AYNI_NOT_READY;
    for (unsigned poll = 0; poll < 16 && written == AYNI_NOT_READY; ++poll) {
        written = port->write(port->context, 1, {value, sizeof(value) - 1}, &reference);
        wifichat::BodyCache::ready(&bodies, 0);
    }
    ASSERT_EQ(written, AYNI_OK);
    ASSERT_TRUE(wifichat::BodyCache::ready(&bodies, 0));
    ASSERT_FALSE(files.values.empty());
    files.removal_capacity = false;
    port->release(port->context, &reference);
    EXPECT_EQ(bodies.reset_idle(), AYNI_BUSY);
    EXPECT_FALSE(files.values.empty());
    files.removal_capacity = true;
    auto status = AYNI_BUSY;
    for (unsigned poll = 0; poll < 16 && status == AYNI_BUSY; ++poll) {
        status = bodies.reset_idle();
    }
    EXPECT_EQ(status, AYNI_OK);
    EXPECT_TRUE(files.values.empty());
}
