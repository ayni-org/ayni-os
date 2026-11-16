#include "history.h"
#include "history_result.h"
#include <algorithm>
#include <cstring>
namespace ayni::strategies::detail {
namespace {
constexpr uint64_t index_magic = UINT64_C(0x314958444e5941); // AYNDXI1, ADR-0104.
constexpr uint32_t word_bytes = 4, wide_bytes = 8, bits_per_byte = 8;
constexpr uint32_t ref_bytes = 24;
constexpr uint32_t raw_body = 0, call_result_view = 1;
void add(std::string &out, uint64_t value, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        out.push_back(static_cast<char>(value));
        value >>= bits_per_byte;
    }
}
bool take(std::string_view &in, uint32_t count, uint64_t &value) {
    if (in.size() < count) {
        return false;
    }
    value = 0;
    for (uint32_t at = 0; at < count; ++at) {
        value |= uint64_t{static_cast<unsigned char>(in[at])} << (at * bits_per_byte);
    }
    in.remove_prefix(count);
    return true;
}
void reference(std::string &out, const BodyRef &ref) {
    add(out, ref.id, wide_bytes);
    add(out, ref.digest, wide_bytes);
    add(out, ref.size, word_bytes);
    add(out, ref.reserved, word_bytes);
}
bool reference(std::string_view &in, BodyRef &ref) {
    uint64_t size = 0, reserved = 0;
    if (!take(in, wide_bytes, ref.id) || !take(in, wide_bytes, ref.digest) ||
        !take(in, word_bytes, size) || !take(in, word_bytes, reserved)) {
        return false;
    }
    ref.size = static_cast<uint32_t>(size);
    ref.reserved = static_cast<uint32_t>(reserved);
    return valid_ref(ref);
}
// Each item owns literal framing followed by a body. Empty body is a literal-only item.
void item(std::string &index, std::string_view literal, const BodyRef &ref,
          uint32_t kind = raw_body) {
    add(index, literal.size(), word_bytes);
    index.append(literal);
    add(index, kind, word_bytes);
    reference(index, ref);
}
// ADR-0104: cold body reads defer independently. Discover dependencies with the
// same index reader before rendering any saved result, so each subsequent wait
// cannot repeat the full original-Schema projection of the already loaded prefix.
ayni_status_t prepare_history(BodyReader &bodies, const BodyRef &ref) {
    std::vector<BodyRef> references;
    std::string ignored;
    auto status = load_history(bodies, ref, ignored, &references);
    if (status != AYNI_OK) {
        return status;
    }
    return bodies.prepare(references);
}
} // namespace
ayni_status_t load_history(BodyReader &bodies, const BodyRef &ref, std::string &out,
                           std::vector<BodyRef> *references) {
    if (!ref.id) {
        out.clear();
        return AYNI_OK;
    }
    if (!references) {
        const auto prepared = prepare_history(bodies, ref);
        if (prepared != AYNI_OK) {
            return prepared;
        }
    }
    BodyReader::Body index;
    auto status = bodies.read(ref, index);
    if (status != AYNI_OK) {
        return status;
    }
    std::string_view input(*index);
    uint64_t magic = 0;
    if (!take(input, wide_bytes, magic) || magic != index_magic) {
        return AYNI_INVALID_ARGUMENT; // The record explicitly declares an index (ADR-0104).
    }
    std::string composed;
    bool pending = false;
    while (!input.empty()) {
        uint64_t size = 0;
        if (!take(input, word_bytes, size) || size > input.size() ||
            input.size() - size < word_bytes + ref_bytes) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (!references) {
            composed.append(input.substr(0, static_cast<size_t>(size)));
        }
        input.remove_prefix(static_cast<size_t>(size));
        uint64_t kind = 0;
        BodyRef body{};
        if (!take(input, word_bytes, kind) || kind > call_result_view || !reference(input, body)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (body.id) {
            if (references) {
                references->push_back(body);
            }
            std::string rendered;
            BodyReader::Body raw;
            std::string_view bytes;
            status = kind == call_result_view ? load_result_view(bodies, body, rendered, references)
                     : references             ? AYNI_OK
                                              : bodies.read(body, raw);
            if (kind == call_result_view) {
                bytes = rendered;
            } else if (raw) {
                bytes = *raw;
            }
            if (status == AYNI_NOT_READY && references) {
                pending = true; // Discover the other independent dependencies in this pass too.
                continue;
            }
            if (status != AYNI_OK) {
                return status;
            }
            if (references) {
                continue;
            }
            if (composed.size() > UINT32_MAX - bytes.size()) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            composed += bytes;
        }
    }
    if (pending) {
        return AYNI_NOT_READY;
    }
    out = std::move(composed);
    return AYNI_OK;
}
// Only new frames are walked. Model proposals retain their original argument JSON; the Core
// Call ledger separately owns the normalized arguments actually authorized and dispatched.
ayni_status_t store_history(const ayni_body_store_v1 &bodies, Turn &turn) {
    if (turn.history.empty()) {
        turn.record.history = {};
        turn.record.history_bytes = 0;
        turn.history_saved = 0;
        turn.history_index.clear();
        return AYNI_OK;
    }
    if (turn.history_saved == turn.history.size()) {
        return AYNI_OK;
    }
    if (!turn.history_saved || turn.history_saved > turn.history.size()) {
        turn.history_index.clear();
        add(turn.history_index, index_magic, wide_bytes);
        turn.history_saved = 0;
    }
    const auto start = turn.history_saved;
    const auto prefix = static_cast<uint32_t>(model::messages_prefix.size());
    if (turn.history.size() < prefix) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::string tail(model::messages_prefix);
    tail.append(turn.history, start ? start : prefix, std::string::npos);
    model::Reader reader(view(tail));
    model::Message message;
    struct Payload {
        ayni_string_v1 bytes;
        BodyRef view{};
    };
    std::vector<Payload> payloads;
    while (reader.next(message)) {
        if (message.text.size) {
            const auto found = message.role == model::Role::tool
                                   ? turn.result_views.find(
                                         std::string(message.tool_id.data, message.tool_id.size))
                                   : turn.result_views.end();
            payloads.push_back(
                {message.text, found == turn.result_views.end() ? BodyRef{} : found->second});
        }
        if (message.continuation.data.size) {
            payloads.push_back({message.continuation.data, {}});
        }
        for (uint32_t at = 0; at < message.count; ++at) {
            if (message.functions[at].arguments.size) {
                payloads.push_back({message.functions[at].arguments, {}});
            }
        }
    }
    if (!reader.finished()) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::sort(payloads.begin(), payloads.end(),
              [](auto a, auto b) { return a.bytes.data < b.bytes.data; });
    std::string appended;
    size_t position = start ? prefix : 0;
    for (const auto payload : payloads) {
        const auto bytes = payload.bytes;
        const auto at = static_cast<size_t>(bytes.data - tail.data());
        BodyRef body = payload.view;
        const auto status = body.id ? AYNI_OK : save_body(bodies, turn.record.id, bytes, body);
        if (status != AYNI_OK) {
            return status;
        }
        item(appended, std::string_view(tail).substr(position, at - position), body,
             payload.view.id ? call_result_view : raw_body);
        position = at + bytes.size;
    }
    if (position < tail.size()) {
        item(appended, std::string_view(tail).substr(position), {});
    }
    std::string candidate = turn.history_index + appended;
    BodyRef ref{};
    const auto status = save_body(bodies, turn.record.id, view(candidate), ref);
    if (status == AYNI_OK) {
        turn.record.history = ref;
        turn.record.history_bytes = static_cast<uint32_t>(turn.history.size());
        turn.history_saved = turn.record.history_bytes;
        turn.history_index = std::move(candidate);
        turn.result_views.clear();
    }
    return status;
}
ayni_status_t record_references(BodyReader &bodies, const ConversationRecord &record,
                                std::vector<BodyRef> &out) {
    Conversation::references(record, out);
    std::string ignored;
    auto status =
        record.history_bytes ? load_history(bodies, record.history, ignored, &out) : AYNI_OK;
    if (status != AYNI_OK || !record.rounds.id) {
        return status;
    }
    status = bodies.copy(record.rounds, ignored);
    if (status != AYNI_OK) {
        return status;
    }
    std::vector<Round> rounds;
    return decode_rounds(bodies, view(ignored), record.round_count, record.last_round, rounds,
                         &out);
}
ayni_status_t load_history(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out,
                           std::vector<BodyRef> *references) {
    BodyReader reader(bodies);
    return load_history(reader, ref, out, references);
}
ayni_status_t record_references(const ayni_body_store_v1 &bodies, const ConversationRecord &record,
                                std::vector<BodyRef> &out) {
    BodyReader reader(bodies);
    return record_references(reader, record, out);
}
} // namespace ayni::strategies::detail
