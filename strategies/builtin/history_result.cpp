#include "history_result.h"
#include <ayni/call_result_json.hpp>
#include <ayni/workspace.hpp>
namespace ayni::strategies::detail {
namespace {
constexpr uint32_t view_magic =
    0x56524e41; // ANRV; layout in contracts/storage/records.md, ADR-0108.
constexpr uint32_t view_format = 2;
constexpr uint32_t word_bytes = 4, wide_bytes = 8, byte_bits = 8;
constexpr uint32_t view_bytes = 152;
struct View {
    ayni_task_call_result_v1 facts{};
    BodyRef value{}, domain{}, proof{}, presentation{};
};
// One field walk defines the reader and writer, including all reference widths.
struct ViewCodec {
    std::string *output = nullptr;
    std::string_view input;
    bool valid = true;
    void number(uint64_t &value, uint32_t bytes) {
        if (output) {
            for (uint32_t at = 0; at < bytes; ++at) {
                output->push_back(static_cast<char>(value >> (at * byte_bits)));
            }
        } else {
            if (input.size() < bytes) {
                valid = false;
                return;
            }
            value = 0;
            for (uint32_t at = 0; at < bytes; ++at) {
                value |= uint64_t{static_cast<unsigned char>(input[at])} << (at * byte_bits);
            }
            input.remove_prefix(bytes);
        }
    }
    void word(uint32_t &value) {
        uint64_t wide = value;
        number(wide, word_bytes);
        value = static_cast<uint32_t>(wide);
    }
    void ref(BodyRef &value) {
        number(value.id, wide_bytes);
        number(value.digest, wide_bytes);
        word(value.size);
        word(value.reserved);
        valid = valid && valid_ref(value);
    }
    void fields(View &view) {
        uint32_t magic = view_magic, format = view_format;
        word(magic);
        word(format);
        valid = valid && magic == view_magic && format == view_format;
        auto &f = view.facts;
        number(f.logical_call_id, wide_bytes);
        number(f.operation_id, wide_bytes);
        number(f.requires_seq, wide_bytes);
        word(f.outcome);
        uint32_t status = static_cast<uint32_t>(f.status);
        word(status);
        f.status = static_cast<ayni_status_t>(status);
        word(f.reason);
        word(f.effect);
        word(f.result_kind); // Explicit canonical VALUE or UTF-8 TEXT.
        uint32_t reserved = 0;
        word(reserved);
        valid = valid && !reserved;
        ref(view.value);
        ref(view.domain);
        ref(view.proof);
        ref(view.presentation);
    }
};
uint32_t result_kind(const ayni_task_call_result_v1 &call) {
    return call.result_kind          ? call.result_kind
           : call.encoded_value.size ? AYNI_CALL_RESULT_VALUE
                                     : AYNI_CALL_RESULT_TEXT;
}
bool same_content(const BodyRef &body, ayni_bytes_v1 bytes) {
    return body.size == bytes.size && body.digest == ayni_body_digest(bytes);
}
// Facts and canonical content/proof identity bind a projection to the currently authorized Call.
bool same_source(const View &view, const ayni_task_call_result_v1 &source) {
    const auto &saved = view.facts;
    if (source.logical_call_id != saved.logical_call_id ||
        source.operation_id != saved.operation_id || source.requires_seq != saved.requires_seq ||
        source.outcome != saved.outcome || source.status != saved.status ||
        source.reason != saved.reason || source.effect != saved.effect ||
        result_kind(source) != saved.result_kind ||
        !same_content(view.domain, source.original_domain) ||
        !same_content(view.proof, source.business_contract)) {
        return false;
    }
    if (source.body.id) {
        return source.body.digest == view.value.digest && source.body.size == view.value.size;
    }
    return same_content(
        view.value,
        saved.result_kind == AYNI_CALL_RESULT_VALUE
            ? source.encoded_value
            : ayni_bytes_v1{reinterpret_cast<const uint8_t *>(source.text.data), source.text.size});
}
// Decode and validate the representation once for history, presentation and source checks.
ayni_status_t read_view(BodyReader &bodies, const BodyRef &ref, View &out) {
    BodyReader::Body encoded;
    const auto status = bodies.read(ref, encoded);
    if (status != AYNI_OK) {
        return status;
    }
    ViewCodec codec{nullptr, *encoded};
    codec.fields(out);
    const auto &f = out.facts;
    const bool terminal = f.outcome == AYNI_CALL_COMPLETED || f.outcome == AYNI_CALL_FAILED ||
                          f.outcome == AYNI_CALL_CANCELLED || f.outcome == AYNI_CALL_SKIPPED;
    return encoded->size() == view_bytes && codec.valid && codec.input.empty() && terminal &&
                   out.value.id && out.domain.id && out.proof.id &&
                   f.status != AYNI_RESULT_UNKNOWN && f.effect != AYNI_CALL_EFFECT_UNKNOWN &&
                   (f.result_kind == AYNI_CALL_RESULT_TEXT ||
                    f.result_kind == AYNI_CALL_RESULT_VALUE)
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}
} // namespace
ayni_status_t store_result_view(const ayni_body_store_v1 &bodies, uint64_t scope,
                                const ayni_task_call_result_v1 &call, BodyRef &out,
                                ayni_string_v1 presentation) {
    if (!call.outcome || call.outcome == AYNI_CALL_ABANDONED ||
        call.status == AYNI_RESULT_UNKNOWN || call.effect == AYNI_CALL_EFFECT_UNKNOWN) {
        return AYNI_INVALID_ARGUMENT;
    }
    View view;
    view.facts = call;
    view.facts.result_kind = result_kind(call);
    auto status =
        call.body.id && bodies.share
            ? bodies.share(bodies.context, scope, &call.body, &view.value)
            : save_body(
                  bodies, scope,
                  view.facts.result_kind == AYNI_CALL_RESULT_VALUE
                      ? ayni_string_v1{reinterpret_cast<const char *>(call.encoded_value.data),
                                       call.encoded_value.size}
                      : call.text,
                  view.value);
    if (status == AYNI_OK && bodies.confirm && bodies.write == bodies.stage) {
        // The chat alias owns its own durable manifest, independently from the Core Call.
        status = bodies.confirm(bodies.context, &view.value);
        if (status == AYNI_NOT_READY) {
            status = AYNI_OK; // Requested only; Session confirms the complete graph before publish.
        }
    }
    if (status == AYNI_OK) {
        status = save_body(
            bodies, scope,
            {reinterpret_cast<const char *>(call.original_domain.data), call.original_domain.size},
            view.domain);
    }
    if (status == AYNI_OK) {
        status = save_body(bodies, scope,
                           {reinterpret_cast<const char *>(call.business_contract.data),
                            call.business_contract.size},
                           view.proof);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (presentation.size) {
        status = save_body(bodies, scope, presentation, view.presentation);
        if (status != AYNI_OK) {
            return status;
        }
    }
    std::string encoded;
    ViewCodec writer{&encoded, {}};
    writer.fields(view);
    if (!writer.valid || encoded.size() != view_bytes) {
        return AYNI_INTERNAL;
    }
    return save_body(bodies, scope, detail::view(encoded), out);
}
ayni_status_t load_result_view(BodyReader &bodies, const BodyRef &ref, std::string &out,
                               std::vector<BodyRef> *references,
                               const ayni_task_call_result_v1 *source) {
    View view;
    auto status = read_view(bodies, ref, view);
    if (status != AYNI_OK) {
        return status;
    }
    if (source && !same_source(view, *source)) {
        return AYNI_REVISION_CONFLICT;
    }
    if (references) {
        for (const auto body : {view.value, view.domain, view.proof, view.presentation}) {
            if (body.id) {
                references->push_back(body);
            }
        }
        return AYNI_OK;
    }
    if (view.presentation.id) {
        return bodies.copy_text(view.presentation, out);
    }
    BodyReader::Body value, domain, proof;
    for (const auto &pair : {std::pair<const BodyRef *, BodyReader::Body *>{&view.value, &value},
                             {&view.domain, &domain},
                             {&view.proof, &proof}}) {
        status = bodies.read(*pair.first, *pair.second);
        if (status != AYNI_OK) {
            return status;
        }
    }
    auto &call = view.facts;
    call.struct_size = sizeof(call);
    call.original_domain = {reinterpret_cast<const uint8_t *>(domain->data()),
                            static_cast<uint32_t>(domain->size())};
    call.business_contract = {reinterpret_cast<const uint8_t *>(proof->data()),
                              static_cast<uint32_t>(proof->size())};
    if (call.result_kind == AYNI_CALL_RESULT_VALUE) {
        call.encoded_value = {reinterpret_cast<const uint8_t *>(value->data()),
                              static_cast<uint32_t>(value->size())};
    } else {
        call.text = {value->data(), static_cast<uint32_t>(value->size())};
    }
    ayni::Workspace workspace;
    status = workspace.create(json::call_result_workspace_requirements);
    if (status != AYNI_OK) {
        return status;
    }
    std::string rendered;
    auto writer = growing_json(rendered);
    status = json::write_call_result(workspace.get(), writer, call);
    size_t bytes = 0;
    if (status != AYNI_OK || !writer.finish(bytes)) {
        return status == AYNI_OK ? AYNI_CAPACITY_EXCEEDED : status;
    }
    rendered.resize(bytes);
    out = std::move(rendered);
    return AYNI_OK;
}
ayni_status_t store_result_presentation(const ayni_body_store_v1 &bodies, uint64_t scope,
                                        const BodyRef &source, ayni_string_v1 presentation,
                                        BodyRef &out) {
    BodyReader bodies_reader(bodies);
    View view;
    auto status = read_view(bodies_reader, source, view);
    if (status != AYNI_OK) {
        return status;
    }
    status = save_body(bodies, scope, presentation, view.presentation);
    if (status != AYNI_OK) {
        return status;
    }
    std::string encoded;
    ViewCodec writer{&encoded, {}};
    writer.fields(view);
    return save_body(bodies, scope, detail::view(encoded), out);
}
ayni_status_t load_result_view(const ayni_body_store_v1 &bodies, const BodyRef &ref,
                               std::string &out, std::vector<BodyRef> *references,
                               const ayni_task_call_result_v1 *source) {
    BodyReader reader(bodies);
    return load_result_view(reader, ref, out, references, source);
}
} // namespace ayni::strategies::detail
