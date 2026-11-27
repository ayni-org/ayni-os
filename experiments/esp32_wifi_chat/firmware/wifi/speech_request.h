#pragma once
#include "../main/speech_profile.h"
#include <ayni/values_json.hpp>
#include <cstddef>
#include <string_view>

namespace wifichat::wifi {
// Ming3's deployment profile owns request encoding; playback still consumes AYTS/1.
inline bool encode_speech_request(std::string_view text, char *buffer, size_t capacity,
                                  size_t &bytes) {
    ayni::json::Writer out(buffer, capacity);
    out.raw("{\"model\":\"ming3-tts\",\"input\":");
    out.string(text);
    out.raw(",\"voice\":\"灵小柔\",\"lang_code\":\"Chinese\",\"response_format\":\"pcm\",\"sample_"
            "rate\":");
    out.unsigned_number(SpeechProfile::rate);
    out.raw("}");
    return out.finish(bytes);
}
} // namespace wifichat::wifi
