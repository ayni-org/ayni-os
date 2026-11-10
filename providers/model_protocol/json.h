// Private bounded JSON boundary shared by request, response and SSE codecs.
#pragma once
#include <ayni/providers/model_protocol.h>
#include <memory>
#include <yyjson.h>
namespace ayni::providers::model_protocol::detail {
using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
Document parse(char *, size_t, char *pool, size_t pool_size, ayni_status_t &,
               bool raw_numbers = false, bool *invalid_usage = nullptr,
               uint32_t maximum_depth = json_depth_max);
// Re-emits validated JSON without growing a private temporary string. Raw numeric lexemes
// stay exact; ordinary floating metadata uses the public JSON writer's finite-number codec.
void write_json(ayni::json::Writer &, yyjson_val *);
} // namespace ayni::providers::model_protocol::detail
