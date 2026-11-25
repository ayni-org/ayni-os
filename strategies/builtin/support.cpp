#include "support.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <charconv>
#include <cstring>
#include <new>

namespace ayni::strategies::detail {
json::Writer growing_json(std::string &output) {
    output.clear();
    return json::Writer::growing(&output, [](void *context, size_t minimum) {
        auto &text = *static_cast<std::string *>(context);
        const auto maximum = text.max_size();
        if (minimum > maximum) {
            return json::Writer::Buffer{};
        }
        // Reuse the string owner's existing capacity policy. resize makes the
        // complete returned byte lane writable, including its currently unused tail.
        text.resize(std::max(minimum, text.capacity()));
        return json::Writer::Buffer{text.data(), text.size()};
    });
}
ayni_string_v1 model_key(uint32_t round, char *buffer) {
    Writer out(buffer, AYNI_CATALOG_ID_MAX_BYTES);
    out.add("model.");
    out.number(round);
    return out.text();
}
ayni_string_v1 function_key(uint64_t source, uint32_t index, char *buffer) {
    Writer out(buffer, AYNI_CATALOG_ID_MAX_BYTES);
    out.add("fn.");
    out.number(source);
    out.add(".");
    out.number(index);
    return out.text();
}

ayni_string_v1 view(std::string_view text) {
    return {text.data(), static_cast<uint32_t>(text.size())};
}
bool equal(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}
bool valid_id(ayni_string_v1 value) {
    if (!value.data || value.size == 0 || value.size > AYNI_CATALOG_ID_MAX_BYTES) {
        return false;
    }
    for (uint32_t i = 0; i < value.size; ++i) {
        const char c = value.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}
// Public component boundary: reject overlong, surrogate and out-of-range UTF-8.
// Core owns its independent ABI validator; no private Core header is imported.
bool valid_text(ayni_string_v1 value) {
    if (value.size && !value.data) {
        return false;
    }
    uint32_t at = 0;
    while (at < value.size) {
        const auto lead = static_cast<unsigned char>(value.data[at++]);
        if (lead < 0x80U) {
            continue;
        }
        uint32_t count = 0, code = 0, minimum = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            count = 1;
            code = lead & 0x1FU;
            minimum = 0x80U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            count = 2;
            code = lead & 0x0FU;
            minimum = 0x800U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            count = 3;
            code = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > value.size - at) {
            return false;
        }
        while (count--) {
            const auto next = static_cast<unsigned char>(value.data[at++]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
    }
    return true;
}
bool valid_call(const Call &call) {
    if (call.mode > AYNI_CALL_MODE_MANAGED ||
        (call.kind == AYNI_POLICY_REQUEST_MODEL && call.mode != AYNI_CALL_MODE_FOREGROUND) ||
        call.preemption > AYNI_CALL_PREEMPT_SUPERSEDE ||
        (call.mode != AYNI_CALL_MODE_FOREGROUND && call.preemption != AYNI_CALL_PREEMPT_NONE)) {
        return false;
    }
    if (call.input) {
        return valid_id(call.binding_id) && call.binding_version != 0U &&
               (call.kind == AYNI_POLICY_REQUEST_CALL || call.kind == AYNI_POLICY_REQUEST_MODEL) &&
               call.arguments.field_count == 0U && call.text.size == 0U &&
               ayni_value_validate(&*call.input) == AYNI_OK;
    }
    return valid_id(call.binding_id) && call.binding_version != 0 &&
           (call.kind == AYNI_POLICY_REQUEST_CALL || call.kind == AYNI_POLICY_REQUEST_MODEL) &&
           call.arguments.struct_size == sizeof(ayni_scope_v1) &&
           call.arguments.field_count <= AYNI_AUTHORITY_FIELD_COUNT_MAX &&
           (call.arguments.field_count == 0 || call.arguments.fields) && valid_text(call.text);
}
bool valid_identity(const Identity &identity) {
    if (!valid_id(identity.id) || identity.version == 0 ||
        identity.reader_count > AYNI_CHECKPOINT_READER_COUNT_MAX ||
        (identity.reader_count && !identity.readers)) {
        return false;
    }
    for (uint32_t i = 0; i < identity.reader_count; ++i) {
        const auto &reader = identity.readers[i];
        if (!valid_id(reader.strategy_id) || reader.min_version == 0 ||
            reader.max_version < reader.min_version) {
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (equal(identity.readers[j].strategy_id, reader.strategy_id)) {
                return false;
            }
        }
    }
    return true;
}
ayni_strategy_v1 descriptor(Identity identity, void *context, ayni_decide_fn decide,
                            uint32_t checkpoint_bytes) {
    ayni_strategy_v1 out{};
    out.struct_size = sizeof(out);
    out.version = identity.version;
    out.strategy_id = identity.id;
    out.ctx = context;
    out.decide = decide;
    out.max_intents_per_call = 1;
    out.intent_size = sizeof(ayni_policy_intent_v1);
    out.max_checkpoint_bytes = checkpoint_bytes;
    out.reader_count = identity.reader_count;
    out.readers = identity.readers;
    return out;
}
ayni_status_t resolve_result_body(const ayni_body_store_v1 &bodies, ayni_task_call_result_v1 &call,
                                  std::string &owned) {
    if (!call.body.id || (call.result_kind == AYNI_CALL_RESULT_VALUE && call.encoded_value.size) ||
        (call.result_kind == AYNI_CALL_RESULT_TEXT && call.text.size)) {
        return AYNI_OK;
    }
    const auto status = get_body(bodies, call.body, owned);
    if (status != AYNI_OK) {
        return status;
    }
    if (call.result_kind == AYNI_CALL_RESULT_VALUE) {
        call.encoded_value = {reinterpret_cast<const uint8_t *>(owned.data()),
                              static_cast<uint32_t>(owned.size())};
    } else if (call.result_kind == AYNI_CALL_RESULT_TEXT) {
        call.text = view(owned);
    } else {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}
ayni_status_t report(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output,
                     ayni_policy_intent_kind_t kind) {
    if (!output.intents || output.intent_capacity == 0) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    output.intents[0] = {};
    output.intents[0].struct_size = sizeof(ayni_policy_intent_v1);
    output.intents[0].kind = kind;
    output.intents[0].expected_revision = snapshot.revision;
    output.intent_count = 1;
    return AYNI_OK;
}
ayni_status_t request(const Call &call, ayni_string_v1 key, ayni_string_v1 text,
                      ayni_policy_output_v1 &output, const ayni_body_store_v1 *bodies,
                      uint64_t scope) {
    if (!output.request_call) {
        return AYNI_UNSUPPORTED;
    }
    BodyRef body{};
    if (bodies && !call.input && call.kind == AYNI_POLICY_REQUEST_MODEL) {
        // A Task may own several model/summary Calls. Their stable step keys give each
        // request an independent body owner, including identical prompts (ADR-0104).
        const auto step_digest =
            ayni_body_digest({reinterpret_cast<const uint8_t *>(key.data), key.size});
        constexpr uint64_t owner_mix = UINT64_C(0x9e3779b97f4a7c15);
        const auto request_scope = step_digest ^ (scope * owner_mix);
        const auto stored = put_body(*bodies, request_scope, text, body);
        if (stored == AYNI_NOT_READY) {
            output.disposition = AYNI_POLICY_OUTPUT_DEFER; // Sent once the body is durable.
        }
        if (stored != AYNI_OK) {
            return stored;
        }
    }
    ayni_call_request_v1 request{};
    request.struct_size = sizeof(request);
    request.kind = call.kind;
    request.binding_version = call.binding_version;
    request.step_key = key;
    request.mode = call.mode;
    request.preemption = call.preemption;
    request.binding_id = call.binding_id;
    ayni::ValueBuilder<AYNI_AUTHORITY_FIELD_COUNT_MAX + 2U> builder;
    if (call.input) {
        request.value = *call.input;
    } else {
        if (call.kind == AYNI_POLICY_REQUEST_MODEL || call.arguments.field_count != 0U) {
            builder.object();
            for (uint32_t at = 0; at < call.arguments.field_count; ++at) {
                const auto &field = call.arguments.fields[at];
                if (field.struct_size != sizeof(field) ||
                    field.allowed.struct_size != sizeof(field.allowed)) {
                    return AYNI_INVALID_ARGUMENT;
                }
                if (field.allowed.kind == AYNI_AUTHORITY_I64) {
                    if (field.allowed.values.integer.min_inclusive !=
                        field.allowed.values.integer.max_inclusive) {
                        return AYNI_INVALID_ARGUMENT;
                    }
                    builder.integer(field.field_id, field.allowed.values.integer.min_inclusive);
                } else if (field.allowed.kind == AYNI_AUTHORITY_ENUM ||
                           field.allowed.kind == AYNI_AUTHORITY_IDENTIFIER) {
                    if (field.allowed.values.tokens.member_count != 1U ||
                        field.allowed.values.tokens.members == nullptr) {
                        return AYNI_INVALID_ARGUMENT;
                    }
                    if (field.allowed.kind == AYNI_AUTHORITY_ENUM) {
                        builder.enumeration(field.field_id, field.allowed.values.tokens.members[0]);
                    } else {
                        builder.identifier(field.field_id, field.allowed.values.tokens.members[0]);
                    }
                } else {
                    return AYNI_INVALID_ARGUMENT;
                }
            }
            if (body.id) {
                builder.body(AYNI_MODEL_BODY_FIELD_ID, body);
            } else {
                builder.text(call.kind == AYNI_POLICY_REQUEST_MODEL ? AYNI_MODEL_BODY_FIELD_ID
                                                                    : UINT32_MAX,
                             text);
            }
            builder.end();
        } else {
            builder.text(0, text);
        }
        const auto built = builder.finish(request.value);
        if (built != AYNI_OK) {
            return built;
        }
    }
    return output.request_call(output.call_context, &request);
}
ayni_status_t result(const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                     ayni_task_call_result_v1 &out) {
    if (!snapshot.call_result) {
        return AYNI_UNSUPPORTED;
    }
    out = {};
    out.struct_size = sizeof(out);
    return snapshot.call_result(snapshot.call_context, key, &out);
}
namespace {
// One complete immutable source is admitted before any parsed proposal is cached.
ayni_status_t parse_model(Workspace &workspace, const ayni_body_store_v1 &bodies,
                          const ayni_task_call_result_v1 &call, PreparedProposals &out) {
    auto owner = PreparedProposals::create();
    auto *parsed = owner.building();
    if (!parsed) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    parsed->call = call.logical_call_id;
    parsed->sequence = call.requires_seq;
    parsed->body = call.body;
    auto status = call.body.id ? get_body(bodies, call.body, parsed->encoded) : AYNI_OK;
    if (!call.body.id) {
        if (!call.text.data || !call.text.size) {
            return AYNI_INVALID_ARGUMENT;
        }
        parsed->encoded.assign(call.text.data, call.text.size);
    }
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t count = 0;
    status = ::ayni::detail::with_workspace(
        workspace, ayni_tool_calls_workspace_requirements, [&](const ayni_workspace_v1 *region) {
            return ayni_tool_calls_read(
                region,
                {reinterpret_cast<const uint8_t *>(parsed->encoded.data()),
                 static_cast<uint32_t>(parsed->encoded.size())},
                parsed,
                [](void *context, uint32_t, const ayni_tool_call_v1 *source) {
                    auto &group = *static_cast<ModelProposals *>(context);
                    auto &saved = group.calls[group.count++];
                    saved.call = *source;
                    if (source->input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
                        const auto &args = source->input.arguments;
                        saved.nodes.reset(new (std::nothrow)
                                              ayni_argument_node_v1[args.node_count]);
                        if (!saved.nodes) {
                            return AYNI_CAPACITY_EXCEEDED;
                        }
                        std::copy_n(args.nodes, args.node_count, saved.nodes.get());
                        saved.call.input.arguments.nodes = nullptr; // No scratch pointer retained.
                    }
                    return AYNI_OK;
                },
                &count, &parsed->assistant);
        });
    if (status != AYNI_OK || count != call.tool_call_count) {
        return status != AYNI_OK ? status : AYNI_INVALID_ARGUMENT;
    }
    out = owner;
    return AYNI_OK;
}
} // namespace
ayni_status_t model_result(Workspace &workspace, const ayni_body_store_v1 &bodies,
                           const ayni_task_snapshot_v1 &snapshot, ayni_string_v1 key,
                           ModelResult &out, model::ReplyEncoding encoding,
                           PreparedProposals *prepared) {
    auto status = result(snapshot, key, out.call);
    auto &call = out.call;
    if (status != AYNI_OK || call.outcome != AYNI_CALL_COMPLETED) {
        return status;
    }
    if (!call.tool_call_count) {
        if (prepared) {
            prepared->reset();
        }
        if (call.body.id) {
            status = get_body(bodies, call.body, out.owned);
            if (status != AYNI_OK) {
                return status;
            }
            call.text = view(out.owned);
            call.body = {};
        }
        return model::read_reply_frame(call.text, encoding, call.text, out.continuation);
    }
    const auto same = [&](const ModelProposals &p) {
        return p.call == call.logical_call_id && p.sequence == call.requires_seq &&
               p.body.id == call.body.id && p.body.digest == call.body.digest &&
               p.body.size == call.body.size && p.body.reserved == call.body.reserved &&
               p.count == call.tool_call_count;
    };
    if (prepared && *prepared && same(**prepared)) {
        out.proposals = *prepared;
    } else {
        status = parse_model(workspace, bodies, call, out.proposals);
        if (status != AYNI_OK) {
            return status;
        }
        if (prepared) {
            *prepared = out.proposals;
        }
    }
    call.body = {};
    return model::read_reply_frame(out.proposals->assistant, encoding, call.text, out.continuation);
}
ayni_status_t outcome(const ayni_task_call_result_v1 &result) {
    if (result.outcome == AYNI_CALL_ABANDONED || result.status == AYNI_RESULT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (result.outcome == 0) {
        return AYNI_BUSY;
    }
    if (result.outcome == AYNI_CALL_COMPLETED && result.status == AYNI_OK) {
        return AYNI_OK;
    }
    return result.status == AYNI_OK ? AYNI_INVALID_STATE : result.status;
}
void Writer::instructions(std::initializer_list<ayni_string_v1> parts) {
    uint64_t size = 0;
    for (const auto &part : parts) {
        size += part.size;
    }
    ok_ = ok_ && size_ == 0; // Only the start of a body can open it.
    add(AYNI_MODEL_INSTRUCTIONS_PREFIX);
    number(size);
    add("\n");
    for (const auto &part : parts) {
        add(part);
    }
}
void Writer::add(ayni_string_v1 text) {
    if (grow_) {
        if (!ok_ || (text.size && !text.data) || text.size > UINT32_MAX - grow_->size()) {
            ok_ = false;
            return;
        }
        grow_->append(text.data, text.size);
        size_ = static_cast<uint32_t>(grow_->size());
        return;
    }
    if (!ok_ || text.size > capacity_ - size_ || (text.size && !text.data)) {
        ok_ = false;
        return;
    }
    if (text.size) {
        std::memcpy(data_ + size_, text.data, text.size);
    }
    size_ += text.size;
}
void Writer::number(uint64_t value) {
    constexpr uint32_t digits = 20;
    char buffer[digits];
    const auto result = std::to_chars(buffer, buffer + digits, value);
    add(ayni_string_v1{buffer, static_cast<uint32_t>(result.ptr - buffer)});
}
void Writer::integer(int64_t value) {
    constexpr uint32_t digits = 21;
    char buffer[digits];
    const auto result = std::to_chars(buffer, buffer + digits, value);
    add(ayni_string_v1{buffer, static_cast<uint32_t>(result.ptr - buffer)});
}
} // namespace ayni::strategies::detail
