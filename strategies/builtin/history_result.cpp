#include "history_result.h"
#include <ayni/call_result_json.hpp>
#include <ayni/workspace.hpp>
namespace ayni::strategies::detail {
namespace {
constexpr uint32_t view_magic = 0x56524e41; // ANRV, format 1; ADR-0104.
constexpr uint32_t view_format = 1;
constexpr uint32_t word_bytes = 4, wide_bytes = 8, byte_bits = 8;
constexpr uint32_t view_bytes = 120;
struct View {
    ayni_task_call_result_v1 facts{};
    BodyRef value{}, domain{}, proof{};
};
// One field walk defines the reader and writer, including all reference widths.
struct Archive {
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
    }
};
} // namespace
ayni_status_t store_result_view(const ayni_body_store_v1 &bodies, uint64_t scope,
                                const ayni_task_call_result_v1 &call, BodyRef &out) {
    if (!bodies.share || !call.body.id || call.outcome != AYNI_CALL_COMPLETED) {
        return AYNI_INVALID_ARGUMENT;
    }
    View view;
    view.facts = call;
    auto status = bodies.share(bodies.context, scope, &call.body, &view.value);
    if (status == AYNI_OK) {
        status = put_body(
            bodies, scope,
            {reinterpret_cast<const char *>(call.original_domain.data), call.original_domain.size},
            view.domain);
    }
    if (status == AYNI_OK) {
        status = put_body(bodies, scope,
                          {reinterpret_cast<const char *>(call.business_contract.data),
                           call.business_contract.size},
                          view.proof);
    }
    if (status != AYNI_OK) {
        return status;
    }
    std::string encoded;
    Archive archive{&encoded, {}};
    archive.fields(view);
    if (!archive.valid || encoded.size() != view_bytes) {
        return AYNI_INTERNAL;
    }
    return put_body(bodies, scope, detail::view(encoded), out);
}
ayni_status_t load_result_view(const ayni_body_store_v1 &bodies, const BodyRef &ref,
                               std::string &out, std::vector<BodyRef> *references) {
    std::string encoded;
    auto status = get_body(bodies, ref, encoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (encoded.size() != view_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    View view;
    Archive archive{nullptr, encoded};
    archive.fields(view);
    if (!archive.valid || !archive.input.empty() || view.facts.outcome != AYNI_CALL_COMPLETED ||
        (view.facts.result_kind != AYNI_CALL_RESULT_TEXT &&
         view.facts.result_kind != AYNI_CALL_RESULT_VALUE)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (references) {
        for (const auto body : {view.value, view.domain, view.proof}) {
            if (body.id) {
                references->push_back(body);
            }
        }
        return AYNI_OK;
    }
    std::string value, domain, proof;
    for (const auto &pair : {std::pair<const BodyRef *, std::string *>{&view.value, &value},
                             {&view.domain, &domain},
                             {&view.proof, &proof}}) {
        status = get_body(bodies, *pair.first, *pair.second);
        if (status != AYNI_OK) {
            return status;
        }
    }
    auto &call = view.facts;
    call.struct_size = sizeof(call);
    call.original_domain = {reinterpret_cast<const uint8_t *>(domain.data()),
                            static_cast<uint32_t>(domain.size())};
    call.business_contract = {reinterpret_cast<const uint8_t *>(proof.data()),
                              static_cast<uint32_t>(proof.size())};
    if (call.result_kind == AYNI_CALL_RESULT_VALUE) {
        call.encoded_value = {reinterpret_cast<const uint8_t *>(value.data()),
                              static_cast<uint32_t>(value.size())};
    } else {
        call.text = detail::view(value);
    }
    ayni::Workspace workspace;
    status = workspace.create(json::call_result_workspace_requirements);
    if (status != AYNI_OK) {
        return status;
    }
    auto measured = json::Writer::measuring();
    status = json::write_call_result(workspace.get(), measured, call);
    size_t bytes = 0;
    if (status != AYNI_OK || !measured.finish(bytes)) {
        return status == AYNI_OK ? AYNI_CAPACITY_EXCEEDED : status;
    }
    out.resize(bytes);
    json::Writer writer(out.data(), bytes);
    status = json::write_call_result(workspace.get(), writer, call);
    return status != AYNI_OK ? status : writer.finish(bytes) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
} // namespace ayni::strategies::detail
