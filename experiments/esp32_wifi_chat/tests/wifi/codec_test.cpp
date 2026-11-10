#include "atvv.h"
#include "capture.h"
#include "configuration.h"
#include "model_codec.h"
#include "model_tools.h"
#include "motor_plan.h"
#include "nvs_flash.h"
#include "settings_codec.h"
#include "value_queue.h"
#include <ayni/strategies/builtin.h>
#include <ayni/values_json.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
// ADR-0097: the independent profile preserves complete tool groups, bounded
// synchronized media, and whole configuration snapshots through its adapters.
using namespace wifichat;
using namespace wifichat::wifi;
TEST(WifiStorage, OuterFilesLeaveRoomForCoreAndReleaseTheirSlotsOnCompletion) {
    // ADR-0097: normal body bursts cannot occupy the whole shared Host queue.
    using namespace ayni::hosts::storage;
    struct Pending {
        std::vector<FileValueRequest> requests;
        static ayni_submit_result_t submit(void *context, const FileValueRequest *request) {
            static_cast<Pending *>(context)->requests.push_back(*request);
            return AYNI_SUBMIT_ACCEPTED;
        }
        static void complete(void *context, const FileValueResult &result) noexcept {
            EXPECT_EQ(result.status, AYNI_OK);
            ++*static_cast<uint32_t *>(context);
        }
    } pending;
    ValueQueue queue;
    ASSERT_TRUE(queue.configure({&pending, Pending::submit}));
    const auto port = queue.port();
    uint32_t completions = 0;
    FileValueRequest request;
    request.context = &completions;
    request.complete = Pending::complete;
    for (uint32_t at = 0; at < ValueQueue::value_capacity; ++at) {
        ASSERT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_ACCEPTED);
    }
    EXPECT_LT(pending.requests.size(), ValueQueue::host_capacity);
    EXPECT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(pending.requests.size(), ValueQueue::value_capacity);
    const auto first = pending.requests.front();
    first.complete(first.context, {AYNI_OK, 0, 0, 0});
    ASSERT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_ACCEPTED);
    for (size_t at = 1; at < pending.requests.size(); ++at) {
        const auto next = pending.requests[at];
        next.complete(next.context, {AYNI_OK, 0, 0, 0});
    }
    EXPECT_EQ(completions, ValueQueue::value_capacity + 1);
}
TEST(WifiRemote, HttControlMayStartBeforeHidWhileCommandedStartNeedsItsPress) {
    // ADR-0097, observed RC003-MS sequence: HTT START precedes HID voice DOWN.
    constexpr uint32_t generation = 7;
    const auto autonomous = atvv::start_epoch({}, generation, true, atvv::htt_reason);
    ASSERT_TRUE(autonomous);
    EXPECT_EQ(*autonomous, generation);
    const std::optional<atvv::VoicePress> pending{{atvv::MicPhase::opening, generation, 0}};
    EXPECT_EQ(atvv::start_epoch(pending, generation, true, 0), generation);
    EXPECT_FALSE(atvv::start_epoch({}, generation, true, 0));
    EXPECT_FALSE(atvv::start_epoch({}, generation, false, atvv::htt_reason));
    // ATVV v1.0 TX close carries the stream ID, not the CTL STOP opcode.
    const auto close = atvv::close_command(3);
    EXPECT_EQ(close[0], 0x0d);
    EXPECT_EQ(close[1], 3);
}
TEST(WifiRemote, CancelledOpeningAndClosingNeverAdmitAnOldStart) {
    // The generation bound before MIC_OPEN survives cancellation and late CTL.
    constexpr uint32_t before_cancel = 7, after_cancel = 8;
    std::optional<atvv::VoicePress> pending{{atvv::MicPhase::opening, before_cancel, 0}};
    EXPECT_FALSE(atvv::start_epoch(pending, after_cancel, true, atvv::htt_reason));
    pending->phase = atvv::MicPhase::closing;
    EXPECT_FALSE(atvv::start_epoch(pending, before_cancel, true, atvv::htt_reason));
}
TEST(WifiRemote, StopContainsAReasonRatherThanAStreamId) {
    // ATVV v1.0: 00 02 means HTT release even when active stream ID is not 2.
    const uint8_t released[] = {0x00, 0x02}, closed[] = {0x00, 0x00}, short_stop[] = {0x00};
    EXPECT_EQ(atvv::stop_reason(released, sizeof(released)), atvv::StopReason::released);
    EXPECT_EQ(atvv::stop_reason(closed, sizeof(closed)), atvv::StopReason::closed);
    EXPECT_FALSE(atvv::stop_reason(short_stop, sizeof(short_stop)));
}
TEST(WifiConfiguration, DeploymentCapacitiesAreAcceptedByCurrentPublicKernel) {
    // ADR-0097: reject an incompatible deployment preset before hardware startup.
    for (const auto input_limit : {AYNI_CALL_INPUT_BYTES_DEFAULT, uint32_t(summary_bytes)}) {
        const auto config = chat_config(input_limit, summary_bytes);
        size_t bytes = 0, alignment = 0;
        ASSERT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_OK);
        EXPECT_GT(bytes, 0U);
        EXPECT_GT(alignment, 0U);
    }
}
TEST(WifiCapture, HighNibbleFirstAndMidstreamSynchronizationArePreserved) {
    // ATVV capture records retain predictor/index resets while HTTP expands PCM.
    std::array<uint8_t, 512> encoded{};
    size_t used = capture::header_bytes;
    const uint8_t initial[] = {0, 0, 0};
    std::array<uint8_t, capture::frame_bytes> first{}, second{};
    first.fill(0x11);
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::sync, initial,
                                sizeof(initial)));
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::audio, first.data(),
                                first.size()));
    const uint8_t reset[] = {0xe8, 0x03, 0};
    ASSERT_TRUE(
        capture::append(encoded.data(), encoded.size(), used, capture::sync, reset, sizeof(reset)));
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::audio, second.data(),
                                second.size()));
    capture::finish(encoded.data(), used, capture::pcm_bytes_per_frame * 2);
    CaptureReader reader;
    ASSERT_TRUE(reader.initialize(encoded.data(), used));
    EXPECT_EQ(reader.pcm_bytes(), capture::pcm_bytes_per_frame * 2);
    std::array<int16_t, capture::frame_samples> samples{};
    size_t count = 0;
    ASSERT_TRUE(reader.next(samples.data(), samples.size(), count));
    EXPECT_EQ(count, capture::frame_samples);
    EXPECT_EQ(samples[0], 1);
    EXPECT_EQ(samples[1], 2);
    ASSERT_TRUE(reader.next(samples.data(), samples.size(), count));
    EXPECT_EQ(samples.front(), 1000);
    EXPECT_EQ(samples.back(), 1000);
    EXPECT_FALSE(reader.next(samples.data(), samples.size(), count));
    EXPECT_TRUE(reader.complete());
}
TEST(WifiCapture, AudioWithoutCodecSynchronizationIsNotUploaded) {
    std::array<uint8_t, 256> encoded{};
    size_t used = capture::header_bytes;
    std::array<uint8_t, capture::frame_bytes> audio{};
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::audio, audio.data(),
                                audio.size()));
    capture::finish(encoded.data(), used, capture::pcm_bytes_per_frame);
    CaptureReader reader;
    EXPECT_FALSE(reader.initialize(encoded.data(), used));
}
TEST(WifiCapture, ThirtySecondRecordingLimitIsIndependentOfLongerSpeechPlayback) {
    // ADR-0097: increasing TTS duration does not expand the ATVV recording contract.
    constexpr uint32_t recording_bytes = 16000 * 30 * sizeof(int16_t);
    std::vector<uint8_t> encoded(capture::capacity);
    size_t used = capture::header_bytes;
    const uint8_t initial[] = {0, 0, 0};
    const std::array<uint8_t, capture::frame_bytes> audio{};
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::sync, initial,
                                sizeof(initial)));
    for (uint32_t bytes = 0; bytes < recording_bytes; bytes += capture::pcm_bytes_per_frame) {
        ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::audio,
                                    audio.data(), audio.size()));
    }
    capture::finish(encoded.data(), used, recording_bytes);
    CaptureReader reader;
    ASSERT_TRUE(reader.initialize(encoded.data(), used));
    EXPECT_EQ(reader.pcm_bytes(), recording_bytes);
    ASSERT_TRUE(capture::append(encoded.data(), encoded.size(), used, capture::audio, audio.data(),
                                audio.size()));
    capture::finish(encoded.data(), used, recording_bytes + capture::pcm_bytes_per_frame);
    EXPECT_FALSE(reader.initialize(encoded.data(), used));
}
TEST(WifiModel, CompleteTextUsesExistingPacketContract) {
    // Wi-Fi profile: final text and summaries retain the existing 4/8 KiB limits.
    Packet packet{};
    ASSERT_TRUE(decode_model(
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"你好"}}]})",
        false, packet));
    EXPECT_EQ(packet.kind, Kind::model_text);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(packet.body.data()), packet.size), "你好");
}
TEST(WifiModel, IncompleteAndAmbiguousResponsesAreNotBusinessSuccess) {
    Packet packet{};
    EXPECT_FALSE(decode_model(
        R"({"choices":[{"finish_reason":"length","message":{"role":"assistant","content":"partial"}}]})",
        false, packet));
    EXPECT_FALSE(decode_model(
        R"({"choices":[],"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"duplicate"}}]})",
        false, packet));
    EXPECT_FALSE(decode_model(std::string(32769, 'x'), false, packet));
}
TEST(WifiSettings, BrowserPatchPreservesOmittedSecretsAndRejectsAmbiguity) {
    SettingsPatch patch{};
    ASSERT_TRUE(parse_settings_patch(
        R"({"speech_url":"http://192.168.1.5:8000","model":"deepseek-flash"})", patch));
    EXPECT_EQ(patch.fields, (1U << 2) | (1U << 5));
    EXPECT_EQ(patch.values.model_key[0], 0);
    EXPECT_FALSE(parse_settings_patch(R"({"model":"one","model":"two"})", patch));
    EXPECT_FALSE(parse_settings_patch(R"({"model_key":"secret\u0000tail"})", patch));
    EXPECT_FALSE(parse_settings_patch(R"({"unknown":"field"})", patch));
}
TEST(WifiSettings, WifiCanBeSavedBeforeBrowserProvisionsModel) {
    // First boot can join the LAN before the user pastes long keys in a browser.
    reset_nvs();
    Settings first{};
    ASSERT_TRUE(first.load());
    std::strcpy(first.ssid.data(), "example");
    std::strcpy(first.password.data(), "password");
    EXPECT_TRUE(first.network_valid());
    EXPECT_FALSE(first.valid());
    ASSERT_TRUE(first.save());
    Settings reopened{};
    ASSERT_TRUE(reopened.load());
    EXPECT_STREQ(reopened.ssid.data(), "example");
    EXPECT_STREQ(reopened.password.data(), "password");
    EXPECT_STREQ(reopened.model.data(), "deepseek-flash");
}
TEST(WifiSettings, CompleteConfigurationUsesOneVersionedSnapshot) {
    reset_nvs();
    Settings first{};
    ASSERT_TRUE(first.load());
    std::strcpy(first.ssid.data(), "example");
    std::strcpy(first.speech_url.data(), "http://192.168.1.5:8000");
    std::strcpy(first.model_key.data(), "test-key");
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(first.save());
    Settings reopened{};
    ASSERT_TRUE(reopened.load());
    EXPECT_TRUE(reopened.valid());
    EXPECT_STREQ(reopened.speech_url.data(), first.speech_url.data());
    EXPECT_STREQ(reopened.model_key.data(), first.model_key.data());
}
TEST(WifiSettings, EndpointAndCredentialSyntaxRejectsHiddenAuthorities) {
    // ADR-0097: only the selected HTTPS model authority receives its credential.
    Settings settings{};
    reset_nvs();
    ASSERT_TRUE(settings.load());
    std::strcpy(settings.ssid.data(), "example");
    std::strcpy(settings.speech_url.data(), "http://192.168.1.5:8000");
    std::strcpy(settings.model_key.data(), "test-key");
    ASSERT_TRUE(settings.valid());
    std::strcpy(settings.model_url.data(), "https://");
    EXPECT_FALSE(settings.valid());
    std::strcpy(settings.model_url.data(), "https://user:secret@host");
    EXPECT_FALSE(settings.valid());
    std::strcpy(settings.model_url.data(), "http://host");
    EXPECT_FALSE(settings.valid());
    EXPECT_EQ(normalized_api_root("https://api.deepseek.com///"), "https://api.deepseek.com");
}

TEST(WifiModel, ActionToolsRoundTripThroughPublicSdkWithOwnedArguments) {
    // ADR-0097: reuse the public complete tool-list codec, keeping arguments alive until post.
    std::string response =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"查询台架","tool_calls":[{"id":"call-1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"capability_id\":\"motor.status\",\"parameters\":{}}"}}]}}]})";
    auto packet = std::make_unique<Packet>();
    EXPECT_FALSE(decode_model(response, false, *packet));
    ASSERT_TRUE(decode_model(response, true, *packet));
    ASSERT_EQ(packet->kind, Kind::model_tools);
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const ayni_workspace_v1 workspace{sizeof(workspace), 0, storage.data(),
                                      storage.size() * sizeof(uint64_t)};
    auto tools = std::make_unique<ModelTools>();
    ASSERT_EQ(tools->decode({packet->body.data(), packet->size}, workspace), AYNI_OK);
    response.clear();
    packet.reset();
    ASSERT_EQ(tools->count(), 1U);
    EXPECT_EQ(std::string_view(tools->assistant().data, tools->assistant().size), "查询台架");
    const auto &call = tools->calls()[0];
    EXPECT_EQ(std::string_view(call.protocol_id.data, call.protocol_id.size), "call-1");
    EXPECT_EQ(std::string_view(call.function_name.data, call.function_name.size),
              "ayni_call_capability");
    ASSERT_EQ(call.input_kind, AYNI_TOOL_INPUT_ARGUMENTS);
    ASSERT_EQ(call.input.arguments.node_count, 3U);
    const auto &value = call.input.arguments.nodes[1].value.text;
    EXPECT_EQ(std::string_view(value.data, value.size), "motor.status");
}

TEST(WifiMotor, ModelCommandsAcceptSixtySecondsAndRetainFinitePwmLimits) {
    // ADR-0097: latest user policy removes per-action approval and bounds a call to 60 s output.
    EXPECT_TRUE(valid_motor_command({90, -90, 60000}));
    EXPECT_TRUE(valid_motor_command({50, 0, 50}));
    EXPECT_TRUE(valid_motor_command({50, 0, 59995}));
    EXPECT_FALSE(valid_motor_command({50, 0, 60005}));
    EXPECT_FALSE(valid_motor_command({91, 0, 1000}));
    EXPECT_FALSE(valid_motor_command({50, 0, 999}));
    EXPECT_EQ(motor_duties(50, -40), (std::array<uint32_t, 4>{50, 0, 0, 40}));
}

TEST(WifiModel, ProgressOutlineDisclosesOnlyProtocolMetadata) {
    // ADR-0097: execution progress is metadata, not private tool arguments or assistant prose.
    const std::string response =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"private-assistant-text","tool_calls":[{"id":"call-1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"speaker.speak_text\",\"binding_version\":1,\"arguments\":{\"text\":\"private-media-text\"}}"}}]}}]})";
    auto packet = std::make_unique<Packet>();
    ASSERT_TRUE(decode_model(response, true, *packet));
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const ayni_workspace_v1 workspace{sizeof(workspace), 0, storage.data(),
                                      storage.size() * sizeof(uint64_t)};
    auto tools = std::make_unique<ModelTools>();
    ASSERT_EQ(tools->decode({packet->body.data(), packet->size}, workspace), AYNI_OK);
    std::array<char, tool_outline_bytes> out{};
    size_t used = 0;
    ASSERT_EQ(tools->outline(out.data(), out.size(), used), AYNI_OK);
    EXPECT_EQ(
        std::string_view(out.data(), used),
        R"([{"function":"ayni_call_capability","binding":"speaker.speak_text","operation":"invoke"}])");
    std::array<char, 1> too_small{};
    EXPECT_EQ(tools->outline(too_small.data(), too_small.size(), used), AYNI_BUFFER_TOO_SMALL);
}

TEST(WifiModel, ProgressOutlineNeverPromotesNestedBusinessFieldsOrUnknownTools) {
    // ADR-0097: unvalidated model proposals cannot smuggle business text into metadata.
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const ayni_workspace_v1 workspace{sizeof(workspace), 0, storage.data(),
                                      storage.size() * sizeof(uint64_t)};
    auto packet = std::make_unique<Packet>();
    auto tools = std::make_unique<ModelTools>();
    for (
        const char *proposal :
        {R"({"id":"1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"arguments\":{\"binding_id\":\"private-nested\"}}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_list_capabilities","arguments":"{\"binding_id\":\"private-list-data\"}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_list_capabilities","arguments":"{\"binding_version\":7070707}"}})",
         R"({"id":"1","type":"function","function":{"name":"private-function-data","arguments":"{\"binding_version\":7070707}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"arguments\":{\"binding_version\":7171717}}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"arguments\":{\"operation\":\"start\"}}"}})",
         R"({"id":"1","type":"function","function":{"name":"private-function-data","arguments":"{\"operation\":\"start\"}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"operation\":\"private-operation-text\"}"}})",
         R"({"id":"1","type":"function","function":{"name":"private-function-data","arguments":"{\"binding_id\":\"private-root-data\"}"}})",
         R"({"id":"1","type":"function","function":{"name":"ayni_inspect_capability","arguments":"{\"binding_id\":\"private invalid\"}"}})"}) {
        const std::string response =
            std::string(
                R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[)") +
            proposal + R"(]}}]})";
        ASSERT_TRUE(decode_model(response, true, *packet));
        ASSERT_EQ(tools->decode({packet->body.data(), packet->size}, workspace), AYNI_OK);
        std::array<char, tool_outline_bytes> out{};
        size_t used = 0;
        ASSERT_EQ(tools->outline(out.data(), out.size(), used), AYNI_OK);
        const std::string_view outline(out.data(), used);
        EXPECT_EQ(outline.find("private"), outline.npos);
        EXPECT_NE(outline.find("\"binding\":\"\""), outline.npos);
        EXPECT_EQ(outline.find("7070707"), outline.npos);
        EXPECT_EQ(outline.find("7171717"), outline.npos);
        EXPECT_EQ(outline.find("\"operation\":\"start\""), outline.npos);
    }
}

TEST(WifiModel, ProgressRetainsTheRootProtocolOperationAfterTheBinding) {
    // ADR-0097: UI distinguishes invoke/start without disclosing private business input.
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const ayni_workspace_v1 workspace{sizeof(workspace), 0, storage.data(),
                                      storage.size() * sizeof(uint64_t)};
    auto packet = std::make_unique<Packet>();
    auto tools = std::make_unique<ModelTools>();
    for (const auto mode : {"invoke", "start", "read", "wait", "cancel", "close"}) {
        const std::string arguments =
            std::string(
                R"({"binding_id":"speaker.speak_text","binding_version":2,"arguments":{"text":"private-media"},"operation":")") +
            mode + R"("})";
        std::array<char, 1024> response{};
        ayni::json::Writer json(response.data(), response.size());
        json.raw(
            R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"1","type":"function","function":{"name":"ayni_call_capability","arguments":)");
        json.string(arguments);
        json.raw(R"(}}]}}]})");
        size_t used = 0;
        ASSERT_TRUE(json.finish(used));
        ASSERT_TRUE(decode_model({response.data(), used}, true, *packet));
        ASSERT_EQ(tools->decode({packet->body.data(), packet->size}, workspace), AYNI_OK);
        std::array<char, tool_outline_bytes> outline{};
        ASSERT_EQ(tools->outline(outline.data(), outline.size(), used), AYNI_OK);
        const std::string_view metadata(outline.data(), used);
        EXPECT_NE(metadata.find(std::string("\"operation\":\"") + mode + '"'), metadata.npos);
        EXPECT_EQ(metadata.find("private"), metadata.npos);
    }
}
