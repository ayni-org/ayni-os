// Native configuration adaptation only; wire behavior belongs to the portable component.
#ifndef AYNI_OPENAI_COMPAT_PROTOCOL_H
#define AYNI_OPENAI_COMPAT_PROTOCOL_H
#include <ayni/providers/model_protocol.h>
#include <ayni/providers/openai_compat.h>
namespace ayni::providers::openai_compat {
using model_protocol::Answer;
inline model_protocol::RequestOptions request_options(const Config &config) {
    model_protocol::RequestOptions options;
    options.model = config.model;
    options.max_output_tokens = config.max_output_tokens;
    options.result_format = config.result_format;
    options.api = config.api;
    options.stream = config.stream;
    options.chat_token_limit = config.chat_token_limit;
    options.reply_encoding = config.reply_encoding;
    options.extra_parameters = config.extra_parameters;
    options.continuation_scope = config.base_url;
    return options;
}
class Protocol : public model_protocol::Protocol {
  public:
    ayni_status_t encode(const Config &config, ayni_string_v1 input, std::string &out) const {
        const auto options = request_options(config);
        return model_protocol::Protocol::encode(options, input, out);
    }
};
} // namespace ayni::providers::openai_compat
#endif
