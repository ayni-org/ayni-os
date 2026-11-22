#include "context_probe.h"
#include "diagnostic_log.h"
#include <ayni/model_messages.hpp>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <yyjson.h>
namespace {
ayni_string_v1 text(std::string_view value) {
    return {value.data(), uint32_t(value.size())};
}
std::string snapshot(wifichat::DiagnosticLog &log, uint64_t after) {
    std::string json(32768, '\0');
    ayni::json::Writer writer(json.data(), json.size());
    EXPECT_TRUE(log.json(writer, after));
    size_t size = 0;
    EXPECT_TRUE(writer.finish(size));
    json.resize(size);
    return json;
}
// ADR-0097: bounded diagnostic retention explicitly reports cursor gaps and boot resets.
TEST(DiagnosticLog, RingCursorReportsOverwrittenEntriesAndHasExactBatchBound) {
    auto log = std::make_unique<wifichat::DiagnosticLog>();
    log->initialize(7, [] { return uint64_t{123}; });
    for (size_t at = 0; at < wifichat::DiagnosticLog::slots + 3; ++at) {
        log->capture("fixed metadata");
    }
    const auto json = snapshot(*log, 0);
    auto *doc = yyjson_read(json.data(), json.size(), 0);
    ASSERT_NE(doc, nullptr);
    auto *root = yyjson_doc_get_root(doc);
    EXPECT_EQ(yyjson_get_uint(yyjson_obj_get(root, "lost")), 3U);
    EXPECT_EQ(yyjson_arr_size(yyjson_obj_get(root, "events")), wifichat::DiagnosticLog::batch);
    EXPECT_TRUE(yyjson_equals_str(yyjson_obj_get(root, "next"), "15"));
    yyjson_doc_free(doc);
    EXPECT_NE(snapshot(*log, UINT64_MAX).find("\"reset\":true"), std::string::npos);
}
// ADR-0097: complete JSON escaping and capture refusal preserve bounded metadata semantics.
TEST(DiagnosticLog, EscapesMetadataAndCountsOversizedCaptureWithoutPublishingATruncation) {
    auto log = std::make_unique<wifichat::DiagnosticLog>();
    log->capture("status=\"-7\"\n");
    log->capture(std::string(wifichat::DiagnosticLog::line_bytes, 'x'));
    const auto json = snapshot(*log, 0);
    auto *doc = yyjson_read(json.data(), json.size(), 0);
    ASSERT_NE(doc, nullptr);
    auto *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events");
    EXPECT_TRUE(
        yyjson_equals_str(yyjson_obj_get(yyjson_arr_get_first(events), "text"), "status=\"-7\"\n"));
    yyjson_doc_free(doc);
    EXPECT_NE(json.find("\"dropped\":1"), std::string::npos);
    EXPECT_EQ(json.find(std::string(100, 'x')), std::string::npos);
    EXPECT_NE(snapshot(*log, 1).find("\"events\":[]"), std::string::npos);
}
// ADR-0097: serial delay and drain do not change the original RAM samples or cursor.
TEST(DiagnosticLog, DeferredToolLinesReachRamOnceAndKeepTheirSamplesThroughSlowSerial) {
    auto log = std::make_unique<wifichat::DiagnosticLog>();
    static uint64_t now = 0;
    log->initialize(7, [] { return now; });
    now = 12;
    log->record_deferred("wifi tool_decision turn=2 task=4 step=fn.8.0 ready_us=%u\n", 12001U);
    now = 15;
    log->record_deferred("wifi tool_dispatch_durable turn=2 call=9 seq=20 at_us=%u\n", 15002U);
    const auto samples = snapshot(*log, 0);
    std::vector<std::string> serial;
    auto slow = [&](const char *line) {
        now += 25;
        serial.emplace_back(line);
        EXPECT_EQ(snapshot(*log, 0), samples); // The sink runs outside the RAM lock.
    };
    log->drain_serial(slow);
    log->drain_serial(slow);
    ASSERT_EQ(serial.size(), 2U);
    EXPECT_EQ(serial[0], "wifi tool_decision turn=2 task=4 step=fn.8.0 ready_us=12001\n");
    EXPECT_EQ(serial[1], "wifi tool_dispatch_durable turn=2 call=9 seq=20 at_us=15002\n");
    EXPECT_NE(samples.find("\"ms\":12"), std::string::npos);
    EXPECT_NE(samples.find("\"ms\":15"), std::string::npos);
    EXPECT_NE(samples.find("\"next\":\"2\""), std::string::npos);
    EXPECT_NE(samples.find("\"dropped\":0"), std::string::npos);
}

// ADR-0097: when an undrained serial copy is overwritten the ring accounts for its loss.
TEST(DiagnosticLog, OverwrittenDeferredLineKeepsCursorAndDropCounts) {
    auto log = std::make_unique<wifichat::DiagnosticLog>();
    log->record_deferred("old deferred metadata\n");
    for (size_t index = 0; index < wifichat::DiagnosticLog::slots; ++index) {
        log->capture("ordinary RAM metadata");
    }
    log->drain_serial([](const char *) { ADD_FAILURE() << "overwritten line reached serial"; });
    const auto samples = snapshot(*log, 0);
    EXPECT_NE(samples.find("\"lost\":1"), std::string::npos);
    EXPECT_NE(samples.find("\"dropped\":1"), std::string::npos);
}

struct Scratch {
    static inline size_t limit = SIZE_MAX;
    static inline const uint8_t *last = nullptr;
    static inline size_t last_size = 0;
    uint8_t *data;
    explicit Scratch(size_t bytes)
        : data(bytes <= limit ? static_cast<uint8_t *>(std::malloc(bytes)) : nullptr) {
        last = data;
        last_size = bytes;
    }
    ~Scratch() { std::free(data); }
};
std::string template_request() {
    constexpr auto definitions =
        R"([{"type":"function","function":{"name":"ayni_call_capability","description":"fixed capability","parameters":{"type":"object","properties":{}}}}])";
    std::string bytes(4096, '\0');
    ayni::model::Writer writer(bytes.data(), bytes.size());
    writer.raw(ayni::model::request_prefix);
    writer.section(text(definitions));
    writer.raw(AYNI_MODEL_INSTRUCTIONS_PREFIX);
    writer.section(text("fixed instructions"));
    writer.prefix();
    writer.text(ayni::model::Role::user, text("PRIVATE_MESSAGE_MUST_NOT_BE_CAPTURED"));
    ayni_string_v1 view{};
    EXPECT_TRUE(writer.finish(view));
    bytes.resize(view.size);
    return bytes;
}
ayni::providers::model_protocol::RequestOptions options() {
    ayni::providers::model_protocol::RequestOptions result;
    result.model = "fixture";
    result.reply_encoding = ayni::model::ReplyEncoding::framed;
    result.continuation_scope = "https://fixture.invalid";
    result.max_output_tokens = wifichat::harness::max_output_tokens;
    return result;
}
// ADR-0098: the capacity lane uses the real selected protocol and never copies chat bodies.
TEST(ContextProbe, BothProtocolsUseOnlyTheTrustedTemplateAndPublishCompleteSyntheticJson) {
    const auto request = template_request();
    auto probe = std::make_unique<wifichat::wifi::ContextProbe>();
    ASSERT_TRUE(probe->capture(text(request)));
    for (auto api : {ayni::providers::model_protocol::Api::chat_completions,
                     ayni::providers::model_protocol::Api::responses}) {
        auto config = options();
        config.api = api;
        bool observed = false;
        const auto result = probe->trial<Scratch>(
            config, wifichat::wifi::ContextProbe::Corpus::han, 4096, [&](const auto &peak) {
                observed = true;
                const std::string_view json(reinterpret_cast<const char *>(Scratch::last),
                                            Scratch::last_size);
                EXPECT_EQ(json.find("PRIVATE_MESSAGE"), std::string_view::npos);
                EXPECT_NE(json.find("fixed instructions"), std::string_view::npos);
                auto *doc = yyjson_read(json.data(), json.size(), 0);
                EXPECT_NE(doc, nullptr);
                if (doc) {
                    yyjson_doc_free(doc);
                }
                EXPECT_GT(peak.scratch_bytes, peak.wire_bytes);
            });
        EXPECT_TRUE(observed);
        EXPECT_EQ(result.status, AYNI_OK);
        EXPECT_GT(result.framed_bytes, 4096U * 3U);
    }
}
// ADR-0095/0098: the configured window and separate framed-byte bound are enforced explicitly.
TEST(ContextProbe, WindowIncludesReservedOutputAndByteRefusalIsSeparateFromMemoryRefusal) {
    auto probe = std::make_unique<wifichat::wifi::ContextProbe>();
    const auto request = template_request();
    ASSERT_TRUE(probe->capture(text(request)));
    auto config = options();
    auto ignore = [](const auto &) {};
    auto fits =
        probe->trial<Scratch>(config, wifichat::wifi::ContextProbe::Corpus::latin, 131072, ignore);
    EXPECT_EQ(fits.status, AYNI_OK);
    auto tokens =
        probe->trial<Scratch>(config, wifichat::wifi::ContextProbe::Corpus::latin, 262144, ignore);
    EXPECT_EQ(tokens.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_STREQ(tokens.stage, "context_budget");
    auto bytes =
        probe->trial<Scratch>(config, wifichat::wifi::ContextProbe::Corpus::han, 262144, ignore);
    EXPECT_STREQ(bytes.stage, "framed_limit");
    Scratch::limit = 1;
    auto memory =
        probe->trial<Scratch>(config, wifichat::wifi::ContextProbe::Corpus::latin, 100, ignore);
    Scratch::limit = SIZE_MAX;
    EXPECT_STREQ(memory.stage, "framed_memory");
    EXPECT_EQ(memory.status, AYNI_CAPACITY_EXCEEDED);
}
} // namespace
