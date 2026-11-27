#include "nvs_flash.h"
#include "protocol.h"
#include "settings_codec.h"
#include "speech_request.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <yyjson.h>
using namespace wifichat;
using namespace wifichat::wifi;

TEST(WifiSpeechRequest, Ming3ProfilePreservesQuotedChineseTextAndAytsFormat) {
    // ADR-0117: Ming3 keeps the AYTS/1 format and the complete caller's text.
    const std::string text = "你好，\"Ayni\"。\n路径 \\ 保留。";
    std::array<char, 2048> buffer{};
    size_t bytes = 0;
    ASSERT_TRUE(encode_speech_request(text, buffer.data(), buffer.size(), bytes));
    auto *doc = yyjson_read(buffer.data(), bytes, 0);
    ASSERT_NE(doc, nullptr);
    auto *root = yyjson_doc_get_root(doc);
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(root, "model")), "ming3-tts");
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(root, "input")), text.c_str());
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(root, "voice")), "灵小柔");
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(root, "lang_code")), "Chinese");
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(root, "response_format")), "pcm");
    EXPECT_EQ(yyjson_get_uint(yyjson_obj_get(root, "sample_rate")), 24000U);
    yyjson_doc_free(doc);
}

TEST(WifiSpeechRequest, FullSpeakerTextFitsBoundedRequestWithoutTruncation) {
    // ADR-0097: request encoding preserves the full bounded speaker input.
    const std::string text(SpeechProfile::text_bytes, '"');
    std::array<char, tx_bytes + 1024> buffer{};
    size_t bytes = 0;
    ASSERT_TRUE(encode_speech_request(text, buffer.data(), buffer.size(), bytes));
    auto *doc = yyjson_read(buffer.data(), bytes, 0);
    ASSERT_NE(doc, nullptr);
    EXPECT_STREQ(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "input")), text.c_str());
    yyjson_doc_free(doc);
    EXPECT_FALSE(encode_speech_request(text, buffer.data(), bytes - 1, bytes));
}

TEST(WifiSettings, TtsPatchLeavesAsrAndCredentialsOmitted) {
    // ADR-0117: the two service roots are independent configuration fields.
    SettingsPatch patch;
    ASSERT_TRUE(parse_settings_patch(R"({"tts_url":"http://192.168.31.221:8080"})", patch));
    EXPECT_EQ(patch.fields, 1U << 9);
    EXPECT_STREQ(patch.values.tts_url.data(), "http://192.168.31.221:8080");
    EXPECT_EQ(patch.values.speech_url[0], 0);
    EXPECT_EQ(patch.values.model_key[0], 0);
    EXPECT_FALSE(parse_settings_patch(R"({"tts_url":"one","tts_url":"two"})", patch));
}

TEST(WifiSettings, SharedSpeechSnapshotKeepsAsrAndSecretsWithNewTtsDefault) {
    // ADR-0117: known settings/v2 is read without rewriting; save publishes v3.
    reset_nvs();
    constexpr size_t header_bytes = 8, checksum_bytes = sizeof(uint32_t);
    constexpr std::array<size_t, 9> widths{33, 65, 192, 192, 192, 64, 17, 6, 22};
    const std::array<const char *, widths.size()> values{"fixture-network",
                                                         "fixture-password",
                                                         "http://192.168.31.162:8000",
                                                         "https://model.example",
                                                         "fixture-key",
                                                         "fixture-model",
                                                         "responses",
                                                         "true",
                                                         "max_completion_tokens"};
    size_t bytes = header_bytes + checksum_bytes;
    for (auto width : widths) {
        bytes += width;
    }
    std::vector<uint8_t> record(bytes);
    put32(record.data(), 0x41594331);
    put32(record.data() + sizeof(uint32_t), 2);
    size_t at = header_bytes;
    for (size_t i = 0; i < widths.size(); ++i) {
        std::memcpy(record.data() + at, values[i], std::strlen(values[i]) + 1);
        at += widths[i];
    }
    put32(record.data() + at, checksum(record.data(), at));
    nvs_handle_t store = 0;
    ASSERT_EQ(nvs_open("ayni_wifi", NVS_READWRITE, &store), ESP_OK);
    ASSERT_EQ(nvs_set_blob(store, "settings", record.data(), record.size()), ESP_OK);
    ASSERT_EQ(nvs_commit(store), ESP_OK);
    Settings loaded;
    ASSERT_TRUE(loaded.load());
    ASSERT_TRUE(loaded.valid());
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_STREQ(loaded.field(i), values[i]);
    }
    EXPECT_STREQ(loaded.tts_url.data(), "http://192.168.31.221:8080");
    std::vector<uint8_t> after(bytes);
    ASSERT_EQ(nvs_get_blob(store, "settings", after.data(), &bytes), ESP_OK);
    EXPECT_EQ(after, record);
    ASSERT_TRUE(loaded.save());
    Settings reopened;
    ASSERT_TRUE(reopened.load());
    for (size_t i = 0; i < Settings::field_count; ++i) {
        EXPECT_STREQ(reopened.field(i), loaded.field(i));
    }
    nvs_close(store);
}

TEST(WifiSettings, BothSpeechRootsUseTheSameUrlValidation) {
    // ADR-0097: neither LAN speech root may hide credentials or another authority.
    Settings settings;
    reset_nvs();
    ASSERT_TRUE(settings.load());
    std::strcpy(settings.ssid.data(), "fixture-network");
    std::strcpy(settings.speech_url.data(), "http://192.168.31.162:8000");
    std::strcpy(settings.model_key.data(), "fixture-key");
    ASSERT_TRUE(settings.valid());
    constexpr const char *invalid[]{"",
                                    "http://",
                                    "http://user:secret@host",
                                    "http://host?q=1",
                                    "file:///tmp/speech",
                                    "http://host with space"};
    for (auto *root : {settings.speech_url.data(), settings.tts_url.data()}) {
        const std::string saved(root);
        for (auto *value : invalid) {
            std::strcpy(root, value);
            EXPECT_FALSE(settings.valid()) << value;
        }
        std::strcpy(root, saved.c_str());
    }
    EXPECT_TRUE(settings.valid());
}
