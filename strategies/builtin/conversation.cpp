#include "support.h"
#include <ayni/detail/model_usage.hpp>
#include <ayni/model_messages.hpp>
#include <cstring>
#include <limits>

namespace ayni::strategies {
namespace {
using namespace detail;
constexpr uint32_t record_magic = 0x434E5941U; // AYNC, little endian.
constexpr uint32_t turn_magic = 0x544E5941U;   // AYNT.
constexpr uint32_t bits_per_byte = 8;
constexpr uint32_t conversation_format = 5;   // Texts are body references (ADR-0096).
constexpr uint32_t canonical_turn_format = 6; // Canonical summary offsets (ADR-0098).
constexpr uint32_t word_bytes = 4;
constexpr uint32_t wide_bytes = 8;

// A single field walk below defines the portable format for reader and writer.
class Encoder {
  public:
    Encoder(uint8_t *data, uint32_t capacity) : data_(data), capacity_(capacity) {}
    void u32(uint32_t value) { integer(value, word_bytes); }
    void u64(uint64_t value) { integer(value, wide_bytes); }
    void ref(const BodyRef &value) {
        u64(value.id);
        u64(value.digest);
        u32(value.size);
        u32(value.reserved);
    }
    void fixed(const char *value, uint32_t bytes) {
        if (!ok_ || bytes > capacity_ - size_) {
            ok_ = false;
            return;
        }
        std::memcpy(data_ + size_, value, bytes);
        size_ += bytes;
    }
    bool ok() const { return ok_; }
    uint32_t size() const { return size_; }

  private:
    void integer(uint64_t value, uint32_t bytes) {
        if (!ok_ || bytes > capacity_ - size_) {
            ok_ = false;
            return;
        }
        for (uint32_t at = 0; at < bytes; ++at) {
            data_[size_++] = static_cast<uint8_t>(value);
            value >>= bits_per_byte;
        }
    }
    uint8_t *data_;
    uint32_t capacity_;
    uint32_t size_ = 0;
    bool ok_ = true;
};
class Decoder {
  public:
    explicit Decoder(ayni_bytes_v1 bytes) : bytes_(bytes) {
        ok_ = bytes.size <= encoded_bytes_max && (bytes.size == 0 || bytes.data);
    }
    void u32(uint32_t &value) { value = static_cast<uint32_t>(integer(word_bytes)); }
    void u64(uint64_t &value) { value = integer(wide_bytes); }
    void ref(BodyRef &value) {
        u64(value.id);
        u64(value.digest);
        u32(value.size);
        u32(value.reserved);
        ok_ = ok_ && valid_ref(value);
    }
    void fixed(char *value, uint32_t bytes) {
        if (!ok_ || bytes > bytes_.size - position_) {
            ok_ = false;
            return;
        }
        std::memcpy(value, bytes_.data + position_, bytes);
        position_ += bytes;
    }
    bool complete() const { return ok_ && position_ == bytes_.size; }

  private:
    uint64_t integer(uint32_t bytes) {
        if (!ok_ || bytes > bytes_.size - position_) {
            ok_ = false;
            return 0;
        }
        uint64_t value = 0;
        for (uint32_t at = 0; at < bytes; ++at) {
            value |= uint64_t{bytes_.data[position_++]} << (at * bits_per_byte);
        }
        return value;
    }
    ayni_bytes_v1 bytes_;
    uint32_t position_ = 0;
    bool ok_ = true;
};
template <typename Archive, typename Context> void context_fields(Archive &a, Context &c) {
    a.u64(c.boot_epoch);
    a.u64(c.owner_instance_id);
    a.u32(c.goal_type_version);
    a.u32(c.goal_type_size);
    a.fixed(c.goal_type_id, AYNI_CATALOG_ID_MAX_BYTES);
    for (auto &word : c.visible) {
        a.u64(word);
    }
    a.u64(c.sample_task_id);
    a.u32(c.sample_count);
    a.u32(c.sample_reserved);
    for (auto &sample : c.samples) {
        a.u64(sample.logical_call_id);
        a.u64(sample.generation);
        a.u64(sample.sequence);
    }
}
template <typename Archive, typename State> void controller_fields(Archive &a, State &s) {
    a.u32(s.model_round);
    a.u32(s.tool_index);
    a.u32(s.group_start);
    a.u32(s.compact_prefix);
    a.u32(s.history_summary.offset);
    a.u32(s.history_summary.chunk);
    a.u32(s.history_summary.next);
    a.u32(s.compact_generation);
    a.ref(s.refs.history_summary);
    a.u64(s.archived_result.reference);
    a.u32(s.archived_result.json_bytes);
    a.u32(s.result_summary.offset);
    a.u32(s.result_summary.next);
    a.u32(s.result_summary.chunk);
    a.ref(s.refs.result_summary);
}
template <typename Archive, typename Record> void fields(Archive &a, Record &r) {
    a.u64(r.id);
    a.u64(r.owner_instance_id);
    a.u64(r.generation);
    a.u64(r.revision);
    a.u64(r.last_round);
    a.u64(r.summary_first);
    a.u64(r.summary_last);
    a.ref(r.summary);
    a.ref(r.draft);
    a.ref(r.history);
    a.u32(r.last_usage.kind);
    a.u32(r.last_usage.reserved);
    a.u64(r.last_usage.input_tokens);
    a.u64(r.last_usage.output_tokens);
    a.u64(r.last_usage.total_tokens);
    a.u64(r.last_request_characters);
    a.u64(r.last_input_estimate);
    context_fields(a, r.disclosure);
    a.u64(r.context_version);
    a.u32(r.round_count);
    a.ref(r.rounds);
}
bool present(const BodyRef &ref) {
    return ref.id != 0U;
}
// Structural record checks; body contents are checked when a reader loads them.
bool valid(const ConversationRecord &r) {
    if (r.disclosure.sample_reserved ||
        r.disclosure.sample_count > AYNI_MODEL_SAMPLE_DEPENDENCY_MAX ||
        (r.disclosure.sample_count ? !r.disclosure.sample_task_id
                                   : r.disclosure.sample_task_id != 0)) {
        return false;
    }
    if (!r.id || !r.owner_instance_id || !r.generation ||
        !::ayni::detail::valid_model_usage(r.last_usage) || !valid_ref(r.summary) ||
        !valid_ref(r.draft) || !valid_ref(r.history) || !valid_ref(r.rounds) ||
        (r.round_count == 0U) != !present(r.rounds) || (present(r.draft) && !r.draft.size)) {
        return false;
    }
    if (r.summary_last == 0) {
        if (r.summary_first || present(r.summary)) {
            return false;
        }
    } else if (r.summary_first == 0 || r.summary_first > r.summary_last ||
               r.summary_last > r.last_round + (r.last_round != UINT64_MAX ? 1U : 0U) ||
               !present(r.summary) || !r.summary.size) {
        return false;
    }
    if (r.generation == 1U && r.context_version == 1U && !present(r.history) &&
        r.last_round >= r.summary_last && r.last_round - r.summary_last != r.round_count) {
        return false;
    }
    if (!r.context_version || r.round_count > r.last_round) {
        return false;
    }
    return !(r.disclosure.goal_type_size > AYNI_CATALOG_ID_MAX_BYTES ||
             (r.disclosure.boot_epoch &&
              (!r.disclosure.owner_instance_id || !r.disclosure.goal_type_version ||
               !r.disclosure.goal_type_size ||
               !valid_text({r.disclosure.goal_type_id, r.disclosure.goal_type_size}))));
}
bool valid_history(const std::string &history) {
    if (history.empty()) {
        return true;
    }
    model::Reader reader(view(history));
    model::Message message;
    while (reader.next(message)) {
    }
    return reader.finished();
}
ayni_status_t header(Decoder &reader, uint32_t expected, uint32_t *version = nullptr) {
    uint32_t magic = 0, format = 0;
    reader.u32(magic);
    reader.u32(format);
    if (magic != expected) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (version) {
        *version = format;
    }
    return format == conversation_format ||
                   (expected == turn_magic && format == canonical_turn_format)
               ? AYNI_OK
               : AYNI_UNSUPPORTED_VERSION;
}

// Decodes references and control state only; no body is read.
ayni_status_t decode_turn(ayni_bytes_v1 bytes, Turn &out) {
    Decoder reader(bytes);
    uint32_t format = 0;
    const auto status = header(reader, turn_magic, &format);
    if (status != AYNI_OK) {
        return status;
    }
    Turn parsed;
    parsed.canonical_summary = format == canonical_turn_format;
    uint32_t done = 0;
    reader.u32(done);
    reader.u64(parsed.base_revision);
    reader.u64(parsed.memory_id);
    fields(reader, parsed.record);
    reader.ref(parsed.refs.input);
    uint32_t phase = 0, resume = 0;
    reader.u32(phase);
    reader.u32(resume);
    controller_fields(reader, parsed);
    if (phase > static_cast<uint32_t>(TurnPhase::result_summary) ||
        resume > static_cast<uint32_t>(TurnPhase::result_summary) ||
        parsed.model_round > max_model_rounds || parsed.tool_index > AYNI_CALL_TOOL_COUNT_MAX ||
        parsed.group_start > parsed.record.history.size ||
        parsed.compact_prefix > parsed.record.history.size ||
        parsed.history_summary.offset > parsed.history_summary.next ||
        (!parsed.canonical_summary && parsed.history_summary.next > parsed.compact_prefix) ||
        parsed.result_summary.offset > parsed.result_summary.next ||
        parsed.result_summary.next > parsed.archived_result.json_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    parsed.phase = static_cast<TurnPhase>(phase);
    parsed.compact_resume = static_cast<TurnPhase>(resume);
    if (!reader.complete() || done > 1 || !valid(parsed.record) || !present(parsed.refs.input) ||
        !parsed.refs.input.size || parsed.base_revision == UINT64_MAX ||
        parsed.record.revision != parsed.base_revision + done) {
        return AYNI_INVALID_ARGUMENT;
    }
    parsed.done = done != 0;
    if (!parsed.done && parsed.record.last_round == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out = std::move(parsed);
    return AYNI_OK;
}
ayni_status_t encode_turn(const Turn &turn, uint8_t *data, uint32_t capacity, uint32_t &size) {
    if (!data || !valid(turn.record) || !present(turn.refs.input) || !turn.refs.input.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoder writer(data, capacity);
    writer.u32(turn_magic);
    writer.u32(turn.canonical_summary ? canonical_turn_format : conversation_format);
    writer.u32(turn.done ? 1U : 0U);
    writer.u64(turn.base_revision);
    writer.u64(turn.memory_id);
    fields(writer, turn.record);
    writer.ref(turn.refs.input);
    writer.u32(static_cast<uint32_t>(turn.phase));
    writer.u32(static_cast<uint32_t>(turn.compact_resume));
    controller_fields(writer, turn);
    if (!writer.ok()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    size = writer.size();
    return AYNI_OK;
}
// An empty text has no body. A text whose reference already names it is not rewritten.
ayni_status_t keep(const ayni_body_store_v1 &bodies, uint64_t scope, const std::string &text,
                   BodyRef &ref) {
    if (text.empty()) {
        ref = {};
        return AYNI_OK;
    }
    const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(text.data()),
                              static_cast<uint32_t>(text.size())};
    if (present(ref) && ayni_body_verify(&ref, bytes) == AYNI_OK) {
        return AYNI_OK;
    }
    return put_body(bodies, scope, view(text), ref);
}
ayni_status_t load(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out) {
    if (!present(ref)) {
        out.clear();
        return AYNI_OK;
    }
    const auto status = get_body(bodies, ref, out);
    return status != AYNI_OK ? status : valid_text(view(out)) ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}
bool valid_rounds(const std::vector<Round> &rounds, uint64_t last_round) {
    uint64_t previous = 0;
    for (const auto &round : rounds) {
        if (round.number <= previous || round.number > last_round || round.user.empty() ||
            !valid_text(view(round.user)) || !valid_text(view(round.answer))) {
            return false;
        }
        previous = round.number;
    }
    return true;
}
} // namespace

namespace detail {
bool valid_ref(const BodyRef &ref) {
    return ref.reserved == 0U && (ref.id != 0U || (ref.digest == 0U && ref.size == 0U));
}
ayni_status_t put_body(const ayni_body_store_v1 &bodies, uint64_t scope, ayni_string_v1 text,
                       BodyRef &out) {
    if (!bodies.write || (text.size && !text.data)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(text.data), text.size};
    BodyRef stored{};
    const auto status = bodies.write(bodies.context, scope, bytes, &stored);
    if (status != AYNI_OK) {
        return status;
    }
    // A store that answers with another body could make Core execute what was not authorized.
    if (ayni_body_verify(&stored, bytes) != AYNI_OK) {
        return AYNI_STORAGE_FAILED;
    }
    out = stored;
    return AYNI_OK;
}
ayni_status_t get_body(const ayni_body_store_v1 &bodies, const BodyRef &ref, std::string &out) {
    if (!bodies.read || !present(ref)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::string bytes(ref.size, '\0');
    const auto status =
        bodies.read(bodies.context, &ref, reinterpret_cast<uint8_t *>(bytes.data()), ref.size);
    if (status != AYNI_OK) {
        return status;
    }
    if (ayni_body_verify(&ref, {reinterpret_cast<const uint8_t *>(bytes.data()), ref.size}) !=
        AYNI_OK) {
        return AYNI_STORAGE_FAILED;
    }
    out = std::move(bytes);
    return AYNI_OK;
}
ayni_status_t encode_rounds(const std::vector<Round> &rounds, std::string &out) {
    out.clear();
    const auto add = [&](uint64_t value, uint32_t bytes) {
        for (uint32_t at = 0; at < bytes; ++at) {
            out.push_back(static_cast<char>(static_cast<uint8_t>(value)));
            value >>= bits_per_byte;
        }
    };
    for (const auto &round : rounds) {
        if (round.user.size() > UINT32_MAX || round.answer.size() > UINT32_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        add(round.number, wide_bytes);
        add(round.user.size(), word_bytes);
        out += round.user;
        add(round.answer.size(), word_bytes);
        out += round.answer;
    }
    return AYNI_OK;
}
ayni_status_t decode_rounds(ayni_string_v1 bytes, uint32_t count, uint64_t last_round,
                            std::vector<Round> &out) {
    std::vector<Round> parsed;
    uint64_t at = 0;
    const auto take = [&](uint32_t width, uint64_t &value) {
        if (width > bytes.size - at) {
            return false;
        }
        value = 0;
        for (uint32_t index = 0; index < width; ++index) {
            value |= uint64_t{static_cast<uint8_t>(bytes.data[at++])} << (index * bits_per_byte);
        }
        return true;
    };
    const auto text = [&](std::string &value) {
        uint64_t size = 0;
        if (!take(word_bytes, size) || size > bytes.size - at) {
            return false;
        }
        value.assign(bytes.data + at, static_cast<std::size_t>(size)); // Bounded by bytes.size.
        at += size;
        return true;
    };
    for (uint32_t index = 0; index < count; ++index) {
        Round round;
        if (!take(wide_bytes, round.number) || !text(round.user) || !text(round.answer)) {
            return AYNI_INVALID_ARGUMENT;
        }
        parsed.push_back(std::move(round));
    }
    if (at != bytes.size || !valid_rounds(parsed, last_round)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = std::move(parsed);
    return AYNI_OK;
}
ayni_status_t read_turn(Workspace &, const ayni_body_store_v1 &bodies, ayni_bytes_v1 bytes,
                        Turn &out) {
    Turn parsed;
    auto status = decode_turn(bytes, parsed);
    if (status != AYNI_OK) {
        return status;
    }
    std::string rounds;
    for (const auto &[ref, text] :
         {std::pair<const BodyRef *, std::string *>{&parsed.refs.input, &parsed.input},
          {&parsed.record.history, &parsed.history},
          {&parsed.record.summary, &parsed.summary},
          {&parsed.record.rounds, &rounds},
          {&parsed.refs.history_summary, &parsed.history_summary.text},
          {&parsed.refs.result_summary, &parsed.result_summary.text}}) {
        status = ref == &parsed.record.rounds
                     ? (present(*ref) ? get_body(bodies, *ref, *text) : AYNI_OK)
                     : load(bodies, *ref, *text);
        if (status != AYNI_OK) {
            return status;
        }
    }
    status = decode_rounds(view(rounds), parsed.record.round_count, parsed.record.last_round,
                           parsed.rounds);
    if (status != AYNI_OK) {
        return status;
    }
    if (!valid_history(parsed.history)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = std::move(parsed);
    return AYNI_OK;
}
ayni_status_t write_turn(Turn &turn, const ayni_body_store_v1 &bodies, uint8_t *data,
                         uint32_t capacity, uint32_t &size) {
    if (turn.input.empty() || !valid_text(view(turn.input)) || !valid_history(turn.history)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto scope = turn.record.id;
    std::string rounds;
    auto status = encode_rounds(turn.rounds, rounds);
    for (const auto &[text, ref] :
         {std::pair<const std::string *, BodyRef *>{&turn.input, &turn.refs.input},
          {&turn.history, &turn.record.history},
          {&turn.summary, &turn.record.summary},
          {&rounds, &turn.record.rounds},
          {&turn.history_summary.text, &turn.refs.history_summary},
          {&turn.result_summary.text, &turn.refs.result_summary}}) {
        if (status != AYNI_OK) {
            return status;
        }
        status = keep(bodies, scope, *text, *ref);
    }
    if (status != AYNI_OK) {
        return status;
    }
    turn.record.round_count = static_cast<uint32_t>(turn.rounds.size());
    return encode_turn(turn, data, capacity, size);
}
} // namespace detail

namespace {
constexpr size_t conversation_alignment_slots = 4;
constexpr size_t conversation_workspace_bytes =
    2 * sizeof(ConversationRecord) + conversation_alignment_slots * Workspace::alignment;
uint64_t scope_of(const ConversationRecord &record) {
    return record.id;
}
ayni_status_t store_rounds(const ayni_body_store_v1 &bodies, ConversationRecord &record,
                           const std::vector<Round> &rounds) {
    std::string encoded;
    auto status = encode_rounds(rounds, encoded);
    if (status == AYNI_OK) {
        status = keep(bodies, scope_of(record), encoded, record.rounds);
    }
    if (status == AYNI_OK) {
        record.round_count = static_cast<uint32_t>(rounds.size());
    }
    return status;
}
} // namespace

static ayni_status_t conversation_begin(const ayni_body_store_v1 &bodies,
                                        const ConversationRecord &record, ayni_string_v1 user,
                                        Encoded &parameters, uint64_t memory_id) {
    if (!valid(record) || !user.data || user.size == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (record.revision == UINT64_MAX || record.last_round == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (!valid_text(user)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Turn turn;
    auto status = load(bodies, record.draft, turn.input);
    if (status != AYNI_OK) {
        return status;
    }
    if (!turn.input.empty()) {
        turn.input.push_back('\n');
    }
    turn.input.append(user.data, user.size);
    turn.record = record;
    turn.record.draft = {};
    turn.base_revision = record.revision;
    turn.memory_id = memory_id;
    status = put_body(bodies, scope_of(record), view(turn.input), turn.refs.input);
    if (status != AYNI_OK) {
        return status;
    }
    Encoded next;
    status = encode_turn(turn, next.bytes.data(), encoded_bytes_max, next.size);
    if (status == AYNI_OK) {
        parameters = next;
    }
    return status;
}
static ayni_status_t conversation_input(ayni_bytes_v1 parameters, BodyRef &out) {
    Turn turn;
    const auto status = decode_turn(parameters, turn);
    if (status == AYNI_OK) {
        out = turn.refs.input;
    }
    return status;
}
static ayni_status_t conversation_finish(const ConversationRecord &current,
                                         ayni_bytes_v1 checkpoint, ConversationRecord &candidate) {
    if (!valid(current)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Turn turn;
    const auto status = decode_turn(checkpoint, turn);
    if (status != AYNI_OK) {
        return status;
    }
    if (!turn.done) {
        return AYNI_BUSY;
    }
    if (turn.record.id != current.id ||
        turn.record.owner_instance_id != current.owner_instance_id ||
        turn.record.generation != current.generation || turn.base_revision != current.revision) {
        return AYNI_REVISION_CONFLICT;
    }
    if (turn.record.last_round != current.last_round + 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    candidate = turn.record;
    return AYNI_OK;
}
static ayni_status_t conversation_retain_published_input(const ayni_body_store_v1 &bodies,
                                                         const ConversationRecord &current,
                                                         uint64_t round, ConversationRecord &out) {
    if (!valid(current) || current.last_round == UINT64_MAX || round != current.last_round + 1 ||
        !present(current.draft)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::vector<Round> rounds;
    std::string draft;
    auto status = Conversation::rounds(bodies, current, rounds);
    if (status == AYNI_OK) {
        status = load(bodies, current.draft, draft);
    }
    if (status != AYNI_OK) {
        return status;
    }
    rounds.push_back({round, std::move(draft), {}});
    ConversationRecord next = current;
    status = store_rounds(bodies, next, rounds);
    if (status != AYNI_OK) {
        return status;
    }
    next.last_round = round;
    next.draft = {};
    out = next;
    return AYNI_OK;
}
static ayni_status_t conversation_erase(const ConversationRecord &current,
                                        ConversationRecord &candidate) {
    if (!valid(current)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (current.generation == UINT64_MAX || current.revision == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    ConversationRecord next{};
    next.id = current.id;
    next.owner_instance_id = current.owner_instance_id;
    next.generation = current.generation + 1U;
    next.revision = current.revision + 1U;
    next.last_round = current.last_round;
    candidate = next;
    return AYNI_OK;
}
// Forgetting any source invalidates the whole generated summary and all answers. Only retained
// user inputs survive; compacted raw turns were already replaced by their now-invalid summary.
// skip names one round to drop as well; zero keeps every retained input.
static ayni_status_t conversation_keep_inputs(const ayni_body_store_v1 &bodies,
                                              const ConversationRecord &current, uint64_t skip,
                                              ConversationRecord &candidate) {
    std::vector<Round> rounds;
    auto status = Conversation::rounds(bodies, current, rounds);
    if (status != AYNI_OK) {
        return status;
    }
    ConversationRecord next{};
    status = conversation_erase(current, next);
    if (status != AYNI_OK) {
        return status;
    }
    std::vector<Round> kept;
    for (auto &round : rounds) {
        if (round.number != skip) {
            kept.push_back({round.number, std::move(round.user), {}});
        }
    }
    next.draft = current.draft;
    status = store_rounds(bodies, next, kept);
    if (status == AYNI_OK) {
        candidate = next;
    }
    return status;
}
static ayni_status_t conversation_encode(const ConversationRecord &record, Encoded &out) {
    if (!valid(record)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoded next;
    Encoder writer(next.bytes.data(), encoded_bytes_max);
    writer.u32(record_magic);
    writer.u32(conversation_format);
    fields(writer, record);
    if (!writer.ok()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    next.size = writer.size();
    out = next;
    return AYNI_OK;
}
static ayni_status_t conversation_decode(ayni_bytes_v1 bytes, ConversationRecord &out) {
    Decoder reader(bytes);
    const auto status = header(reader, record_magic);
    if (status != AYNI_OK) {
        return status;
    }
    ConversationRecord parsed{};
    fields(reader, parsed);
    if (!reader.complete() || !valid(parsed)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = parsed;
    return AYNI_OK;
}

ayni_status_t Conversation::workspace_requirements(size_t *bytes, size_t *alignment) {
    if (!bytes || !alignment) {
        return AYNI_INVALID_ARGUMENT;
    }
    *bytes = conversation_workspace_bytes;
    *alignment = Workspace::alignment;
    return AYNI_OK;
}
namespace {
ayni_status_t checked(const ayni_workspace_v1 *region) {
    return Workspace::check(region, conversation_workspace_bytes);
}
} // namespace

ayni_status_t Conversation::begin(const ayni_workspace_v1 *region, const ayni_body_store_v1 &bodies,
                                  const ConversationRecord &record, ayni_string_v1 user,
                                  Encoded &parameters, uint64_t memory_id) {
    const auto status = checked(region);
    return status != AYNI_OK ? status
                             : conversation_begin(bodies, record, user, parameters, memory_id);
}
ayni_status_t Conversation::input(const ayni_workspace_v1 *region, ayni_bytes_v1 parameters,
                                  BodyRef &out) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_input(parameters, out);
}
ayni_status_t Conversation::finish(const ayni_workspace_v1 *region,
                                   const ConversationRecord &current, ayni_bytes_v1 checkpoint,
                                   ConversationRecord &candidate) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_finish(current, checkpoint, candidate);
}
ayni_status_t Conversation::retain_published_input(const ayni_workspace_v1 *region,
                                                   const ayni_body_store_v1 &bodies,
                                                   const ConversationRecord &current,
                                                   uint64_t round, ConversationRecord &out) {
    const auto status = checked(region);
    return status != AYNI_OK ? status
                             : conversation_retain_published_input(bodies, current, round, out);
}
ayni_status_t Conversation::erase(const ayni_workspace_v1 *region,
                                  const ConversationRecord &current,
                                  ConversationRecord &candidate) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_erase(current, candidate);
}
ayni_status_t Conversation::erase_round(const ayni_workspace_v1 *region,
                                        const ayni_body_store_v1 &bodies,
                                        const ConversationRecord &current, uint64_t round,
                                        ConversationRecord &candidate) {
    auto status = checked(region);
    if (status != AYNI_OK) {
        return status;
    }
    if (round == 0 || round > current.last_round) {
        return AYNI_NOT_FOUND;
    }
    return valid(current) ? conversation_keep_inputs(bodies, current, round, candidate)
                          : AYNI_INVALID_ARGUMENT;
}
ayni_status_t Conversation::erase_derived(const ayni_workspace_v1 *region,
                                          const ayni_body_store_v1 &bodies,
                                          const ConversationRecord &current,
                                          ConversationRecord &candidate) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_keep_inputs(bodies, current, 0, candidate);
}
ayni_status_t Conversation::encode(const ayni_workspace_v1 *region,
                                   const ConversationRecord &record, Encoded &out) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_encode(record, out);
}
ayni_status_t Conversation::decode(const ayni_workspace_v1 *region, ayni_bytes_v1 bytes,
                                   ConversationRecord &out) {
    const auto status = checked(region);
    return status != AYNI_OK ? status : conversation_decode(bytes, out);
}
ayni_status_t Conversation::validate_turn(void *context, ayni_bytes_v1 parameters) {
    auto status = checked(static_cast<const ayni_workspace_v1 *>(context));
    if (status != AYNI_OK) {
        return status;
    }
    Turn turn;
    status = decode_turn(parameters, turn);
    return status != AYNI_OK ? status : turn.done ? AYNI_INVALID_ARGUMENT : AYNI_OK;
}
ayni_status_t Conversation::text(const ayni_body_store_v1 &bodies, const BodyRef &ref,
                                 std::string &out) {
    return valid_ref(ref) ? load(bodies, ref, out) : AYNI_INVALID_ARGUMENT;
}
ayni_status_t Conversation::rounds(const ayni_body_store_v1 &bodies,
                                   const ConversationRecord &record, std::vector<Round> &out) {
    if (!valid(record)) {
        return AYNI_INVALID_ARGUMENT;
    }
    std::string encoded;
    if (present(record.rounds)) {
        const auto status = get_body(bodies, record.rounds, encoded);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return decode_rounds(view(encoded), record.round_count, record.last_round, out);
}
void Conversation::references(const ConversationRecord &record, std::vector<BodyRef> &out) {
    for (const auto *ref : {&record.summary, &record.history, &record.draft, &record.rounds}) {
        if (present(*ref)) {
            out.push_back(*ref);
        }
    }
}
ayni_status_t Conversation::turn_references(const ayni_workspace_v1 *region, ayni_bytes_v1 bytes,
                                            std::vector<BodyRef> &out) {
    auto status = checked(region);
    if (status != AYNI_OK) {
        return status;
    }
    Turn turn;
    status = decode_turn(bytes, turn);
    if (status != AYNI_OK) {
        return status;
    }
    references(turn.record, out);
    for (const auto *ref :
         {&turn.refs.input, &turn.refs.history_summary, &turn.refs.result_summary}) {
        if (present(*ref)) {
            out.push_back(*ref);
        }
    }
    return AYNI_OK;
}
} // namespace ayni::strategies
