#pragma once
#include "../main/harness.h"
#include "../main/synthetic_history.h"
#include <algorithm>
#include <array>
#include <ayni/model_messages.hpp>
#include <ayni/providers/model_protocol.h>

namespace wifichat::wifi {
// Synthetic transport-capacity probe, isolated from history, Core and HTTP model calls.
// Only trusted request instructions/definitions are retained; message bodies are never copied.
class ContextProbe {
  public:
    using Corpus = SyntheticHistory::Corpus;
    struct Sample {
        uint32_t units = 0;
        size_t framed_bytes = 0, wire_bytes = 0, scratch_bytes = 0;
        uint64_t estimated_tokens = 0;
        ayni_status_t status = AYNI_OK;
        const char *stage = "ready";
    };
    bool ready() const { return prefix_size_ != 0; }
    bool capture(ayni_string_v1 request) {
        if (ready()) {
            return true; // Frozen deployment; retain one trusted template, no history copies.
        }
        ayni_string_v1 messages{}, definitions{}, instructions{};
        if (ayni::model::split_request(request, messages, definitions) != AYNI_OK ||
            !definitions.size ||
            ayni::model::split_instructions(messages, instructions, messages) != AYNI_OK) {
            return false;
        }
        ayni::model::Writer out(prefix_.data(), prefix_.size());
        out.raw(ayni::model::request_prefix);
        out.section(definitions);
        out.raw(AYNI_MODEL_INSTRUCTIONS_PREFIX);
        out.section(instructions);
        out.prefix();
        ayni_string_v1 result{};
        if (!out.finish(result)) {
            return false;
        }
        prefix_size_ = result.size;
        return true;
    }
    static const char *name(Corpus corpus) {
        switch (corpus) {
        case Corpus::latin:
            return "latin";
        case Corpus::han:
            return "han";
        case Corpus::escaped:
            return "escaped";
        }
        return "unsupported";
    }
    // Scratch is the deployment's exclusive byte lane (PSRAM on ESP, heap in Native tests).
    // observe runs while both lanes exist, so the caller can sample the real transport peak.
    // This does not measure ToolLoop history/compaction, TLS, cloud tokenization or latency.
    template <class Scratch, class Observe>
    Sample trial(ayni::providers::model_protocol::RequestOptions &options, Corpus corpus,
                 uint32_t units, Observe observe) const {
        Sample result;
        result.units = units;
        if (!ready()) {
            result.status = AYNI_NOT_READY;
            result.stage = "template";
            return result;
        }
        const auto unit_bytes = SyntheticHistory::unit_bytes(corpus);
        const uint64_t upper = prefix_size_ + SyntheticHistory::room(corpus, units);
        if (!units || upper > UINT32_MAX) {
            result.status = AYNI_INVALID_ARGUMENT;
            result.stage = "input";
            return result;
        }
        if (prefix_size_ + uint64_t(units) * unit_bytes > harness::model_body_bytes) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.stage = "framed_limit";
            return result;
        }
        Scratch framed{size_t(upper)};
        if (!framed.data) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.stage = "framed_memory";
            return result;
        }
        ayni::model::Writer frame(reinterpret_cast<char *>(framed.data), uint32_t(upper));
        frame.raw(std::string_view(prefix_.data(), prefix_size_));
        SyntheticHistory::append(frame, corpus, units);
        ayni_string_v1 context{};
        if (!frame.finish(context) || context.size > harness::model_body_bytes) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.stage = "framed_limit";
            return result;
        }
        result.framed_bytes = context.size;
        const auto projection = ayni::providers::model_protocol::Protocol::projection(options);
        ayni::model::TokenEstimate estimate;
        result.status = ayni::model::estimate_input(projection, context,
                                                    ayni::model::Mode::assistant, estimate);
        result.estimated_tokens = estimate.tokens;
        if (result.status != AYNI_OK) {
            result.stage = "projection";
            return result;
        }
        if (estimate.tokens > harness::context_window_tokens - harness::max_output_tokens) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.stage = "context_budget";
            return result;
        }
        const ayni::providers::model_protocol::Protocol codec;
        auto measure = ayni::json::Writer::measuring();
        result.status = codec.write_request(measure, options, context);
        if (result.status != AYNI_OK || !measure.finish(result.wire_bytes) ||
            result.wire_bytes > harness::model_body_bytes) {
            result.status = result.status == AYNI_OK ? AYNI_CAPACITY_EXCEEDED : result.status;
            result.stage = "wire_limit";
            return result;
        }
        Scratch wire{result.wire_bytes};
        if (!wire.data) {
            result.status = AYNI_CAPACITY_EXCEEDED;
            result.stage = "wire_memory";
            return result;
        }
        ayni::json::Writer out(reinterpret_cast<char *>(wire.data), result.wire_bytes);
        result.status = codec.write_request(out, options, context);
        size_t written = 0;
        if (result.status == AYNI_OK && (!out.finish(written) || written != result.wire_bytes)) {
            result.status = AYNI_CAPACITY_EXCEEDED;
        }
        result.stage = result.status == AYNI_OK ? "ready" : "encode";
        result.scratch_bytes = size_t(upper) + result.wire_bytes;
        observe(result);
        return result;
    }

  private:
    static constexpr size_t prefix_capacity = 32768;
    std::array<char, prefix_capacity> prefix_{};
    uint32_t prefix_size_ = 0;
};
} // namespace wifichat::wifi
