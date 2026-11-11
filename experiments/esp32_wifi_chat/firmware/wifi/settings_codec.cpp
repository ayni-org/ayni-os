#include "settings_codec.h"
#include <cstring>
#include <yyjson.h>
namespace wifichat::wifi {
bool parse_settings_patch(std::string_view body, SettingsPatch &out) {
    constexpr size_t maximum_bytes = 2048;
    constexpr const char *names[] = {"ssid",  "password", "speech_url", "model_url",   "model_key",
                                     "model", "api_type", "stream",     "output_limit"};
    if (body.empty() || body.size() > maximum_bytes) {
        return false;
    }
    auto *doc = yyjson_read(body.data(), body.size(), 0);
    if (!doc) {
        return false;
    }
    SettingsPatch patch{};
    auto *root = yyjson_doc_get_root(doc);
    bool ok = yyjson_is_obj(root) && yyjson_obj_size(root) <= Settings::field_count;
    size_t at, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, at, count, key, value) {
        size_t index = 0;
        while (index < Settings::field_count && !yyjson_equals_str(key, names[index])) {
            ++index;
        }
        if (index == Settings::field_count || !yyjson_is_str(value) ||
            (patch.fields & (1U << index)) ||
            yyjson_get_len(value) >= patch.values.capacity(index)) {
            ok = false;
            break;
        }
        const auto length = yyjson_get_len(value);
        if (std::memchr(yyjson_get_str(value), 0, length)) {
            ok = false;
            break;
        }
        std::memcpy(patch.values.field(index), yyjson_get_str(value), length);
        patch.values.field(index)[length] = 0;
        patch.fields |= 1U << index;
    }
    yyjson_doc_free(doc);
    if (ok && patch.fields && patch.values.model_options_valid()) {
        out = patch;
    } else {
        ok = false;
    }
    patch.values.model_key.fill(0);
    patch.values.password.fill(0);
    return ok;
}
} // namespace wifichat::wifi
