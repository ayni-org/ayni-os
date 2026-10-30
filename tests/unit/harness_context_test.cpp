// ADR-0085: normal context-budget fallback and chunked result handling.
#include "strategies_fixture.h"
#include <ayni/call_result_json.hpp>
#include <memory>
namespace {
using namespace ayni::test;
using History = policies::Text<policies::history_bytes_max>;
// A bounded synchronous Host fixture. SQLite append/erasure is tested separately;
// this lane verifies real Core Calls and policy sequencing without external I/O.
struct ContextMemory {
    struct View {
        History history;
        uint64_t owner = 0, conversation = 0, generation = 0;
    };
    struct Result {
        static constexpr uint32_t capacity = 1048576;
        std::array<char, capacity> json{};
        uint64_t call = 0, sequence = 0;
        uint32_t size = 0, outcome = 0, effect = 0;
    };
    std::array<View, 16> views{};
    std::array<Result, 8> results{};
    uint32_t view_count = 0, result_count = 0;
    bool defer_completed_save_once = true;
    policies::HistoryPort port() { return {this, ready, load, save, archive, chunk}; }
    static uint32_t ready(void *, uint64_t) {
        return 1;
    } // Simulates completion before Core checks ready.
    static bool same(const History &a, const History &b) {
        return a.size == b.size &&
               (!a.size || !std::memcmp(a.bytes.data(), b.bytes.data(), a.size));
    }
    static ayni_status_t load(void *context, uint64_t, const policies::ConversationRecord &record,
                              History &out) {
        auto &self = *static_cast<ContextMemory *>(context);
        if (!record.history_ref || record.history_ref > self.view_count) {
            return AYNI_NOT_FOUND;
        }
        const auto &view = self.views[record.history_ref - 1];
        if (view.owner != record.owner_instance_id || view.conversation != record.id ||
            view.generation != record.generation) {
            return AYNI_DENIED;
        }
        out = view.history;
        return AYNI_OK;
    }
    static ayni_status_t save(void *context, uint64_t, uint64_t,
                              const policies::ConversationRecord &record, uint64_t &reference) {
        auto &self = *static_cast<ContextMemory *>(context);
        for (uint32_t at = 0; at < self.view_count; ++at) {
            const auto &view = self.views[at];
            if (view.owner == record.owner_instance_id && view.conversation == record.id &&
                view.generation == record.generation && same(view.history, record.history)) {
                reference = at + 1U;
                return AYNI_OK;
            }
        }
        if (self.view_count == self.views.size()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &view = self.views[self.view_count++];
        view = {record.history, record.owner_instance_id, record.id, record.generation};
        reference = self.view_count;
        if (self.defer_completed_save_once) {
            self.defer_completed_save_once = false;
            return AYNI_NOT_READY;
        }
        return AYNI_OK;
    }
    static ayni_status_t archive(void *context, uint64_t, const policies::ConversationRecord &,
                                 const ayni_task_call_result_v1 &call,
                                 policies::ArchivedResult &out) {
        auto &self = *static_cast<ContextMemory *>(context);
        for (uint32_t at = 0; at < self.result_count; ++at) {
            if (self.results[at].call == call.logical_call_id &&
                self.results[at].sequence == call.requires_seq) {
                out = {at + 1U, self.results[at].size};
                return AYNI_OK;
            }
        }
        if (self.result_count == self.results.size()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &result = self.results[self.result_count];
        const uint64_t reference = self.result_count + 1U;
        ayni::json::Writer writer(result.json.data(), static_cast<uint32_t>(result.json.size()));
        const auto status = ayni::json::write_call_result(writer, call, reference);
        size_t size = 0;
        if (status != AYNI_OK || !writer.finish(size)) {
            return status != AYNI_OK ? status : AYNI_CAPACITY_EXCEEDED;
        }
        result.call = call.logical_call_id;
        result.sequence = call.requires_seq;
        result.size = static_cast<uint32_t>(size);
        result.outcome = call.outcome;
        result.effect = call.effect;
        ++self.result_count;
        out = {reference, result.size};
        return AYNI_OK;
    }
    static ayni_status_t chunk(void *context, uint64_t, uint64_t reference, uint32_t offset,
                               policies::Text<policies::prompt_bytes_max> &out, uint32_t maximum) {
        auto &self = *static_cast<ContextMemory *>(context);
        if (!reference || reference > self.result_count) {
            return AYNI_NOT_FOUND;
        }
        const auto &source = self.results[reference - 1];
        if (offset > source.size) {
            return AYNI_INVALID_ARGUMENT;
        }
        out = {};
        out.size = std::min({maximum, source.size - offset, policies::prompt_bytes_max});
        while (out.size && offset + out.size < source.size &&
               (static_cast<unsigned char>(source.json[offset + out.size]) & 0xc0U) == 0x80U) {
            --out.size;
        }
        std::memcpy(out.bytes.data(), source.json.data() + offset, out.size);
        return AYNI_OK;
    }
};
// ADR-0085: archive and chunk access form one Host-owned lifecycle.
TEST(ContextConfiguration, rejects_incomplete_archive_callbacks_and_history_lifecycle) {
    const auto memory = std::make_unique<ContextMemory>();
    policies::ToolLoopConfig config;
    config.identity = {text("loop"), 1, nullptr, 0};
    config.model = {AYNI_POLICY_REQUEST_MODEL,
                    text("model"),
                    1,
                    {sizeof(ayni_scope_v1), 0, nullptr},
                    text("Use tools.")};
    config.summary = {
        AYNI_POLICY_REQUEST_MODEL, text("summary"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    const auto full = memory->port();
    auto rejects = [&](policies::HistoryPort incomplete) {
        config.history = incomplete;
        policies::ToolLoop loop;
        EXPECT_EQ(loop.configure(config), AYNI_INVALID_ARGUMENT);
    };
    auto incomplete = full;
    incomplete.result_chunk = nullptr;
    rejects(incomplete);
    incomplete = full;
    incomplete.archive = nullptr;
    rejects(incomplete);
    incomplete = full;
    incomplete.ready = nullptr;
    rejects(incomplete);
    incomplete = full;
    incomplete.load = nullptr;
    rejects(incomplete);
    incomplete = full;
    incomplete.save = nullptr;
    rejects(incomplete);
    config.history = full;
    policies::ToolLoop loop;
    EXPECT_EQ(loop.configure(config), AYNI_OK);
}
class ContextHarness : public StrategiesFixture {
  protected:
    ContextHarness() { history_port = storage.port(); }
    void TearDown() override {
        StrategiesFixture::TearDown();
        EXPECT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    }
    ContextMemory storage;
};
TEST_F(ContextHarness, completed_save_race_does_not_pause_or_repeat_a_model_call) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    post(0, text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_NE(conversation.history_ref, 0U);
}
TEST_F(ContextHarness, oversized_executed_result_uses_chunk_summaries_and_keeps_known_facts) {
    const auto task = start("chat", "one operation", 32);
    propose(0, 1);
    const std::string original(4000, '\x01');
    post(1, {original.data(), static_cast<uint32_t>(original.size())});
    pump();
    uint32_t summaries = 0;
    while (pending.back().capability == "summary") {
        ASSERT_LT(summaries++, 24U);
        post(static_cast<uint32_t>(pending.size() - 1), text("tentative result summary"));
        pump();
    }
    EXPECT_GT(summaries, 1U);
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find("unconfirmed_summary"), std::string::npos);
    EXPECT_NE(pending.back().text.find("result_ref"), std::string::npos);
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &p) { return p.capability == "echo"; }),
              1);
    ASSERT_GE(storage.result_count, 2U);
    EXPECT_EQ(storage.results[1].outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(storage.results[1].effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_GT(storage.results[1].size, original.size());
    post(static_cast<uint32_t>(pending.size() - 1), text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(ContextHarness, declared_summary_failure_pauses_without_changing_the_executed_result) {
    const auto task = start("chat", "one operation", 32);
    propose(0, 1);
    const std::string original(4000, '\x01');
    post(1, {original.data(), static_cast<uint32_t>(original.size())});
    pump();
    ASSERT_EQ(pending.back().capability, "summary");
    post(static_cast<uint32_t>(pending.size() - 1), {}, AYNI_CALL_FAILED, AYNI_NOT_READY);
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_NOT_READY);
    ASSERT_GE(storage.result_count, 2U);
    EXPECT_EQ(storage.results[1].outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(storage.results[1].effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_GT(storage.results[1].size, original.size());
    EXPECT_EQ(std::count_if(pending.begin(), pending.end(),
                            [](const auto &p) { return p.capability == "echo"; }),
              1);
}
TEST_F(StrategiesFixture, absent_service_usage_uses_a_labelled_character_estimate) {
    service_usage = {};
    const auto task = start("chat", "你好 abc");
    post(0, text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(conversation.last_usage.kind, AYNI_MODEL_USAGE_NONE);
    EXPECT_GT(conversation.last_request_characters, 0U);
    EXPECT_EQ(conversation.last_input_estimate,
              ayni::model::estimate_tokens(conversation.last_request_characters));
}
TEST(ContextEstimate, counts_unicode_characters_and_rounds_the_selected_ratio_up) {
    EXPECT_EQ(ayni::model::estimate_tokens(4), 3U);
    EXPECT_EQ(ayni::model::estimate_tokens(5), 4U);
    char frame[256];
    ayni::model::Writer messages(frame, sizeof(frame));
    messages.prefix();
    messages.text(ayni::model::Role::user, {"中文😀", 10});
    ayni_string_v1 body{};
    ASSERT_TRUE(messages.finish(body));
    auto projected = ayni::json::Writer::measuring();
    ASSERT_EQ(ayni::model::write_context(projected, body, ayni::model::Mode::text), AYNI_OK);
    size_t bytes = 0;
    ASSERT_TRUE(projected.finish(bytes));
    EXPECT_EQ(bytes - projected.characters(), 7U); // Two three-byte and one four-byte code points.
}
} // namespace
