#include "support.h"
#include <cstring>
#include <limits>

namespace ayni::strategies {
namespace {
using namespace detail;
constexpr uint32_t record_magic = 0x434E5941U; // AYNC, little endian.
constexpr uint32_t turn_magic = 0x544E5941U;   // AYNT.
constexpr uint32_t bits_per_byte = 8;
constexpr uint32_t conversation_format =
    2; // Sparse retained turns and monotone deletion high-water.
constexpr uint32_t word_bytes = 4;
constexpr uint32_t wide_bytes = 8;

// A single field walk below defines the portable format for reader and writer.
class Encoder {
  public:
    Encoder(uint8_t *data, uint32_t capacity) : data_(data), capacity_(capacity) {}
    void u32(uint32_t value) { integer(value, word_bytes); }
    void u64(uint64_t value) { integer(value, wide_bytes); }
    template <uint32_t N> void text(const Text<N> &value) {
        u32(value.size);
        if (value.size > N || value.size > capacity_ - size_) {
            ok_ = false;
            return;
        }
        if (ok_ && value.size) {
            std::memcpy(data_ + size_, value.bytes.data(), value.size);
        }
        size_ += value.size;
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
        ok_ = bytes.size <= state_bytes_max && (bytes.size == 0 || bytes.data);
    }
    void u32(uint32_t &value) { value = static_cast<uint32_t>(integer(word_bytes)); }
    void u64(uint64_t &value) { value = integer(wide_bytes); }
    template <uint32_t N> void text(Text<N> &value) {
        u32(value.size);
        if (!ok_ || value.size > N || value.size > bytes_.size - position_) {
            ok_ = false;
            return;
        }
        if (value.size) {
            std::memcpy(value.bytes.data(), bytes_.data + position_, value.size);
        }
        position_ += value.size;
        ok_ = valid_text(value.view());
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
template <typename Archive, typename Record> void fields(Archive &a, Record &r) {
    a.u64(r.id);
    a.u64(r.owner_instance_id);
    a.u64(r.generation);
    a.u64(r.revision);
    a.u64(r.last_round);
    a.u64(r.summary_first);
    a.u64(r.summary_last);
    a.text(r.summary);
    a.text(r.draft);
    a.u32(r.round_count);
    for (uint32_t at = 0; at < r.round_count && at < retained_rounds_max; ++at) {
        auto &round = r.rounds[at];
        a.u64(round.number);
        a.text(round.user);
        a.text(round.answer);
    }
}
bool valid(const ConversationRecord &r) {
    if (!r.id || !r.owner_instance_id || !r.generation || r.round_count > retained_rounds_max ||
        r.summary.size > summary_text_max || !valid_text(r.summary.view()) ||
        r.draft.size > turn_text_max || !valid_text(r.draft.view())) {
        return false;
    }
    if (r.summary_last == 0) {
        if (r.summary_first || r.summary.size) {
            return false;
        }
    } else if (r.summary_first == 0 || r.summary_first > r.summary_last ||
               r.summary_last > r.last_round || !r.summary.size) {
        return false;
    }
    if (r.last_round < r.summary_last ||
        (r.generation == 1 && r.last_round - r.summary_last != r.round_count)) {
        return false;
    }
    for (uint32_t at = 0; at < r.round_count; ++at) {
        const auto &round = r.rounds[at];
        const auto previous = at == 0 ? r.summary_last : r.rounds[at - 1].number;
        if (round.number <= previous || round.number > r.last_round || round.user.size == 0 ||
            round.user.size > turn_text_max || round.answer.size > turn_text_max ||
            !valid_text(round.user.view()) || !valid_text(round.answer.view())) {
            return false;
        }
    }
    return true;
}
ayni_status_t header(Decoder &reader, uint32_t expected) {
    uint32_t magic = 0, format = 0;
    reader.u32(magic);
    reader.u32(format);
    if (magic != expected) {
        return AYNI_INVALID_ARGUMENT;
    }
    return format == conversation_format ? AYNI_OK : AYNI_UNSUPPORTED_VERSION;
}
} // namespace

namespace detail {
ayni_status_t read_turn(ayni_bytes_v1 bytes, Turn &out) {
    Decoder reader(bytes);
    const auto status = header(reader, turn_magic);
    if (status != AYNI_OK) {
        return status;
    }
    Turn parsed;
    uint32_t done = 0;
    reader.u32(done);
    reader.u64(parsed.base_revision);
    reader.u64(parsed.memory_id);
    fields(reader, parsed.record);
    reader.text(parsed.input);
    if (!reader.complete() || done > 1 || !valid(parsed.record) || parsed.input.size == 0 ||
        parsed.base_revision == UINT64_MAX ||
        parsed.record.revision != parsed.base_revision + done) {
        return AYNI_INVALID_ARGUMENT;
    }
    parsed.done = done != 0;
    if (!parsed.done && parsed.record.last_round == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out = parsed;
    return AYNI_OK;
}
ayni_status_t write_turn(const Turn &turn, uint8_t *data, uint32_t capacity, uint32_t &size) {
    if (!data || !valid(turn.record) || !turn.input.size || turn.input.size > turn_text_max ||
        !valid_text(turn.input.view())) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoder writer(data, capacity);
    writer.u32(turn_magic);
    writer.u32(conversation_format);
    writer.u32(turn.done ? 1U : 0U);
    writer.u64(turn.base_revision);
    writer.u64(turn.memory_id);
    fields(writer, turn.record);
    writer.text(turn.input);
    if (!writer.ok()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    size = writer.size();
    return AYNI_OK;
}
} // namespace detail

ayni_status_t Conversation::begin(const ConversationRecord &record, ayni_string_v1 user,
                                  Encoded &parameters, uint64_t memory_id) {
    if (!valid(record) || !user.data || user.size == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (user.size > turn_text_max || record.revision == UINT64_MAX ||
        record.last_round == UINT64_MAX || record.round_count >= retained_rounds_max) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (!valid_text(user)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const uint32_t separator = record.draft.size ? 1U : 0U;
    if (user.size + record.draft.size + separator > turn_text_max) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Turn turn;
    turn.record = record;
    turn.record.draft = {};
    turn.base_revision = record.revision;
    turn.memory_id = memory_id;
    turn.input = record.draft;
    if (separator) {
        turn.input.bytes[turn.input.size++] = '\n';
    }
    std::memcpy(turn.input.bytes.data() + turn.input.size, user.data, user.size);
    turn.input.size += user.size;
    Encoded next;
    const auto status = write_turn(turn, next.bytes.data(), state_bytes_max, next.size);
    if (status == AYNI_OK) {
        parameters = next;
    }
    return status;
}
ayni_status_t Conversation::input(ayni_bytes_v1 parameters, Text<turn_text_max> &out) {
    Turn turn;
    const auto status = read_turn(parameters, turn);
    if (status == AYNI_OK) {
        out = turn.input;
    }
    return status;
}
ayni_status_t Conversation::finish(const ConversationRecord &current, ayni_bytes_v1 checkpoint,
                                   ConversationRecord &candidate) {
    if (!valid(current)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Turn turn;
    const auto status = read_turn(checkpoint, turn);
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
ayni_status_t Conversation::retain_published_input(const ConversationRecord &current,
                                                   uint64_t round, ConversationRecord &out) {
    if (!valid(current) || current.last_round == UINT64_MAX || round != current.last_round + 1 ||
        !current.draft.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (current.round_count == retained_rounds_max) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto next = current;
    next.rounds[next.round_count++] = {round, current.draft, {}};
    next.last_round = round;
    next.draft = {};
    out = next;
    return AYNI_OK;
}
ayni_status_t Conversation::erase(const ConversationRecord &current,
                                  ConversationRecord &candidate) {
    if (!valid(current)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (current.generation == UINT64_MAX || current.revision == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    ConversationRecord next;
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
ayni_status_t Conversation::erase_derived(const ConversationRecord &current,
                                          ConversationRecord &candidate) {
    auto status = erase(current, candidate);
    if (status != AYNI_OK) {
        return status;
    }
    candidate.draft = current.draft;
    candidate.round_count = current.round_count;
    for (uint32_t index = 0; index < current.round_count; ++index) {
        candidate.rounds[index].number = current.rounds[index].number;
        candidate.rounds[index].user = current.rounds[index].user;
    }
    return AYNI_OK;
}
ayni_status_t Conversation::erase_round(const ConversationRecord &current, uint64_t round,
                                        ConversationRecord &candidate) {
    if (round == 0 || round > current.last_round) {
        return AYNI_NOT_FOUND;
    }
    ConversationRecord next;
    const auto status = erase_derived(current, next);
    if (status != AYNI_OK) {
        return status;
    }
    next.round_count = 0;
    for (uint32_t index = 0; index < current.round_count; ++index) {
        if (current.rounds[index].number != round) {
            next.rounds[next.round_count++] = {
                current.rounds[index].number, current.rounds[index].user, {}};
        }
    }
    for (uint32_t index = next.round_count; index < retained_rounds_max; ++index) {
        next.rounds[index] = {};
    }
    candidate = next;
    return AYNI_OK;
}
ayni_status_t Conversation::encode(const ConversationRecord &record, Encoded &out) {
    if (!valid(record)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Encoded next;
    Encoder writer(next.bytes.data(), state_bytes_max);
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
ayni_status_t Conversation::decode(ayni_bytes_v1 bytes, ConversationRecord &out) {
    Decoder reader(bytes);
    const auto status = header(reader, record_magic);
    if (status != AYNI_OK) {
        return status;
    }
    ConversationRecord parsed;
    fields(reader, parsed);
    if (!reader.complete() || !valid(parsed)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = parsed;
    return AYNI_OK;
}
ayni_status_t Conversation::validate_turn(void *, ayni_bytes_v1 parameters) {
    Turn turn;
    const auto status = read_turn(parameters, turn);
    return status != AYNI_OK ? status : turn.done ? AYNI_INVALID_ARGUMENT : AYNI_OK;
}
} // namespace ayni::strategies
