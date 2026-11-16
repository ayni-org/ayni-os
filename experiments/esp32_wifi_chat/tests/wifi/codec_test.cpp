#include "atvv.h"
#include "capture.h"
#include "configuration.h"
#include "model_codec.h"
#include "model_tools.h"
#include "motor_plan.h"
#include "nvs_flash.h"
#include "settings_codec.h"
#include <ayni/hosts/storage/value_admission.h>
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
    constexpr uint32_t value_capacity = 8, host_capacity = 16;
    ValueAdmission<value_capacity> queue;
    ASSERT_TRUE(queue.configure({&pending, Pending::submit}));
    const auto port = queue.port();
    uint32_t completions = 0;
    FileValueRequest request;
    request.context = &completions;
    request.complete = Pending::complete;
    for (uint32_t at = 0; at < value_capacity; ++at) {
        ASSERT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_ACCEPTED);
    }
    EXPECT_LT(pending.requests.size(), host_capacity);
    EXPECT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(pending.requests.size(), value_capacity);
    const auto first = pending.requests.front();
    first.complete(first.context, {AYNI_OK, 0, 0, 0});
    ASSERT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_ACCEPTED);
    for (size_t at = 1; at < pending.requests.size(); ++at) {
        const auto next = pending.requests[at];
        next.complete(next.context, {AYNI_OK, 0, 0, 0});
    }
    EXPECT_EQ(completions, value_capacity + 1);
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
    EXPECT_EQ(packet.kind, Kind::model_result);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(packet.body.data() + model_result_header_bytes),
                          packet.size - model_result_header_bytes),
              "你好");
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

TEST(WifiModel, MultipleToolsRoundTripThroughPublicSdkWithOwnedArguments) {
    // ADR-0103: retain a whole model batch and its argument ownership through the ESP codec.
    std::string response =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"查询台架","tool_calls":[{"id":"call-1","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"capability_id\":\"motor.status\",\"parameters\":{}}"}},{"id":"call-2","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"capability_id\":\"motor.status\",\"parameters\":{}}"}}]}}]})";
    auto packet = std::make_unique<Packet>();
    EXPECT_FALSE(decode_model(response, false, *packet));
    ASSERT_TRUE(decode_model(response, true, *packet));
    ASSERT_EQ(packet->kind, Kind::model_result);
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> storage((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    const ayni_workspace_v1 workspace{sizeof(workspace), 0, storage.data(),
                                      storage.size() * sizeof(uint64_t)};
    auto tools = std::make_unique<ModelTools>();
    ASSERT_EQ(tools->decode({packet->body.data() + model_result_header_bytes,
                             packet->size - uint32_t(model_result_header_bytes)},
                            workspace),
              AYNI_OK);
    response.clear();
    packet.reset();
    ASSERT_EQ(tools->count(), 2U);
    EXPECT_EQ(std::string_view(tools->assistant().data, tools->assistant().size), "查询台架");
    const auto &second = tools->calls()[1];
    EXPECT_EQ(std::string_view(second.protocol_id.data, second.protocol_id.size), "call-2");
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
    EXPECT_EQ(motor_duties(50, -40), (std::array<uint32_t, 4>{50, 0, 40, 0}));
}

TEST(WifiMotor, BoardCalibrationReversesOnlySecondMotorAndPreservesCoast) {
    // ADR-0097, 2026-11-13: only motor 2 changes polarity; plans and output share motor_input.
    EXPECT_EQ(motor_pins[motor_input(0, 50)], 38U);
    EXPECT_EQ(motor_pins[motor_input(0, -50)], 39U);
    EXPECT_EQ(motor_pins[motor_input(1, 50)], 41U);
    EXPECT_EQ(motor_pins[motor_input(1, -50)], 40U);
    EXPECT_EQ(motor_duties(50, 0), (std::array<uint32_t, 4>{50, 0, 0, 0}));
    EXPECT_EQ(motor_duties(-50, 0), (std::array<uint32_t, 4>{0, 50, 0, 0}));
    EXPECT_EQ(motor_duties(0, 50), (std::array<uint32_t, 4>{0, 0, 0, 50}));
    EXPECT_EQ(motor_duties(0, -50), (std::array<uint32_t, 4>{0, 0, 50, 0}));
    EXPECT_EQ(motor_duties(90, 90), (std::array<uint32_t, 4>{90, 0, 0, 90}));
    EXPECT_EQ(motor_duties(-90, -90), (std::array<uint32_t, 4>{0, 90, 90, 0}));
    EXPECT_EQ(motor_duties(0, 0), (std::array<uint32_t, 4>{0, 0, 0, 0}));
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
    ASSERT_EQ(tools->decode({packet->body.data() + model_result_header_bytes,
                             packet->size - uint32_t(model_result_header_bytes)},
                            workspace),
              AYNI_OK);
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
        ASSERT_EQ(tools->decode({packet->body.data() + model_result_header_bytes,
                                 packet->size - uint32_t(model_result_header_bytes)},
                                workspace),
                  AYNI_OK);
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
        ASSERT_EQ(tools->decode({packet->body.data() + model_result_header_bytes,
                                 packet->size - uint32_t(model_result_header_bytes)},
                                workspace),
                  AYNI_OK);
        std::array<char, tool_outline_bytes> outline{};
        ASSERT_EQ(tools->outline(outline.data(), outline.size(), used), AYNI_OK);
        const std::string_view metadata(outline.data(), used);
        EXPECT_NE(metadata.find(std::string("\"operation\":\"") + mode + '"'), metadata.npos);
        EXPECT_EQ(metadata.find("private"), metadata.npos);
    }
}

TEST(WifiSettings, ApiAndStreamingOptionsPersistWithoutChangingSavedSecrets) {
    // ADR-0098: one atomic versioned configuration, loaded by the next boot.
    reset_nvs();
    Settings first;
    ASSERT_TRUE(first.load());
    std::strcpy(first.ssid.data(), "example");
    std::strcpy(first.model_key.data(), "fixture-key");
    std::strcpy(first.api_type.data(), "responses");
    std::strcpy(first.stream.data(), "true");
    std::strcpy(first.output_limit.data(), "max_completion_tokens");
    ASSERT_TRUE(first.save());
    Settings reopened;
    ASSERT_TRUE(reopened.load());
    EXPECT_STREQ(reopened.api_type.data(), "responses");
    EXPECT_STREQ(reopened.stream.data(), "true");
    EXPECT_STREQ(reopened.output_limit.data(), "max_completion_tokens");
    EXPECT_STREQ(reopened.model_key.data(), "fixture-key");
    SettingsPatch patch;
    EXPECT_FALSE(parse_settings_patch(R"({"api_type":"guess"})", patch));
    EXPECT_FALSE(parse_settings_patch(R"({"stream":"yes"})", patch));
    EXPECT_TRUE(parse_settings_patch(R"({"api_type":"responses","stream":"true"})", patch));
}
TEST(WifiSettings, UnsupportedSettingsAreRefusedWithoutRewritingCredentials) {
    // ADR-0108: only settings/v2 is supported; refusal preserves the stored blob.
    reset_nvs();
    Settings first;
    std::strcpy(first.ssid.data(), "fixture-network");
    std::strcpy(first.model_key.data(), "fixture-key");
    ASSERT_TRUE(first.save());
    nvs_handle_t store = 0;
    ASSERT_EQ(nvs_open("ayni_wifi", NVS_READWRITE, &store), ESP_OK);
    size_t size = 0;
    ASSERT_EQ(nvs_get_blob(store, "settings", nullptr, &size), ESP_OK);
    std::vector<uint8_t> bytes(size);
    ASSERT_EQ(nvs_get_blob(store, "settings", bytes.data(), &size), ESP_OK);
    put32(bytes.data() + sizeof(uint32_t), 1);
    put32(bytes.data() + size - sizeof(uint32_t), checksum(bytes.data(), size - sizeof(uint32_t)));
    ASSERT_EQ(nvs_set_blob(store, "settings", bytes.data(), size), ESP_OK);
    ASSERT_EQ(nvs_commit(store), ESP_OK);
    Settings loaded;
    EXPECT_FALSE(loaded.load());
    std::vector<uint8_t> after(size);
    ASSERT_EQ(nvs_get_blob(store, "settings", after.data(), &size), ESP_OK);
    EXPECT_EQ(after, bytes);
    nvs_close(store);
}
TEST(WifiModel, ResponsesStoredReplyKeepsContinuationAndUsageOutOfVisibleText) {
    // ADR-0098: the ESP queue carries the same selected reply encoding as Native.
    using namespace ayni::providers::model_protocol;
    RequestOptions options;
    options.model = "fixture";
    options.api = Api::responses;
    options.continuation_scope = "route";
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    const std::string response =
        R"({"status":"completed","output":[{"type":"reasoning","id":"rs_1","summary":[],"encrypted_content":"opaque"},{"type":"message","id":"msg_1","status":"completed","role":"assistant","content":[{"type":"output_text","text":"visible"}]}],"usage":{"input_tokens":21,"output_tokens":3,"total_tokens":24}})";
    auto packet = std::make_unique<Packet>();
    ASSERT_TRUE(decode_model(response, true, *packet, options));
    ASSERT_EQ(packet->kind, Kind::model_result);
    EXPECT_EQ(get32(packet->body.data()), 0U);
    EXPECT_EQ(get64(packet->body.data() + 8), 21U);
    ayni_string_v1 text_value;
    ayni::model::Continuation state;
    ASSERT_EQ(ayni::model::read_reply_frame(
                  {reinterpret_cast<const char *>(packet->body.data() + model_result_header_bytes),
                   packet->size - uint32_t(model_result_header_bytes)},
                  options.reply_encoding, text_value, state),
              AYNI_OK);
    EXPECT_EQ(std::string_view(text_value.data, text_value.size), "visible");
    EXPECT_NE(std::string_view(state.data.data, state.data.size).find("opaque"),
              std::string_view::npos);
}

TEST(WifiModel, CompleteOutputRefusalKeepsSharedStatusReasonAndEffect) {
    // C-PROV-004/ADR-0098: a complete malformed output is a classified refusal, not transport
    // UNKNOWN.
    Packet packet;
    EXPECT_FALSE(decode_model(
        R"({"choices":[{"finish_reason":"length","message":{"role":"assistant","content":"partial"}}]})",
        true, packet));
    ASSERT_EQ(packet.kind, Kind::model_error);
    ASSERT_EQ(packet.size, model_result_header_bytes);
    EXPECT_EQ(get32(packet.body.data() + 32), AYNI_CALL_FAILED);
    EXPECT_EQ(static_cast<int32_t>(get32(packet.body.data() + 36)), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(get32(packet.body.data() + 40), AYNI_REASON_MODEL_OUTPUT);
    EXPECT_EQ(get32(packet.body.data() + 44), AYNI_CALL_EFFECT_UNKNOWN);
    model_failure(packet, AYNI_RESULT_UNKNOWN, AYNI_REASON_MODEL_OUTPUT);
    EXPECT_EQ(get32(packet.body.data() + 32), AYNI_CALL_ABANDONED);
    EXPECT_EQ(get32(packet.body.data() + 40), AYNI_REASON_NONE);
}
