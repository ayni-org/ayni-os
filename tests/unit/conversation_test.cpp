#include "strategy_workspace_fixture.h"
// ADR-0056: caller-owned, portable values. No external persistence is implied.
#include <array>
#include <ayni/model_messages.hpp>
#include <ayni/runtime/common/memory_bodies.h>
#include <ayni/strategies/builtin.h>
#include <ayni/values.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace {
using namespace ayni::strategies;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
ConversationRecord empty() {
    ConversationRecord record;
    record.id = 11;
    record.owner_instance_id = 22;
    return record;
}
ayni::runtime::MemoryBodies &memory() {
    static ayni::runtime::MemoryBodies bodies;
    return bodies;
}
const ayni_body_store_v1 &bodies() {
    return *memory().store();
}
BodyRef body(const std::string &text) {
    BodyRef ref{};
    const ayni_bytes_v1 bytes{reinterpret_cast<const uint8_t *>(text.data()),
                              static_cast<uint32_t>(text.size())};
    EXPECT_EQ(bodies().write(bodies().context, empty().id, bytes, &ref), AYNI_OK);
    return ref;
}
std::string read(const BodyRef &ref) {
    std::string out;
    EXPECT_EQ(Conversation::text(bodies(), ref, out), AYNI_OK);
    return out;
}
// The text of a model request, which the ToolLoop names by reference (ADR-0096).
bool model_body(const ayni_call_request_v1 &request, std::string &out) {
    ayni::ValueReader value;
    ayni_body_ref_v1 ref{};
    ayni_string_v1 text{};
    if (value.read(request.value) != AYNI_OK) {
        return false;
    }
    const auto *field = value.field(AYNI_MODEL_BODY_FIELD_ID);
    if (ayni::ValueReader::body(field, ref)) {
        return Conversation::text(bodies(), ref, out) == AYNI_OK;
    }
    if (!ayni::ValueReader::text(field, text)) {
        return false;
    }
    out.assign(text.data, text.size);
    return true;
}
// Retained rounds as one body, the way the ToolLoop publishes them.
void retain(ConversationRecord &record, uint32_t count, uint32_t bytes) {
    std::string encoded;
    for (uint32_t at = 0; at < count; ++at) {
        const uint64_t number = record.last_round - count + 1U + at;
        const std::string user(bytes, 'u'), answer(bytes, 'a');
        for (uint32_t b = 0; b < 8; ++b) {
            encoded.push_back(static_cast<char>((number >> (8U * b)) & 0xffU));
        }
        for (const auto *text : {&user, &answer}) {
            const auto size = static_cast<uint32_t>(text->size());
            for (uint32_t b = 0; b < 4; ++b) {
                encoded.push_back(static_cast<char>((size >> (8U * b)) & 0xffU));
            }
            encoded += *text;
        }
    }
    record.rounds = body(encoded);
    record.round_count = count;
}
ToolLoopConfig loop_config() {
    ToolLoopConfig config;
    config.identity.id = text("loop");
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, text("model"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    config.limits = {32768, 1024, 8192, 4096};
    config.bodies = &bodies();
    return config;
}
TEST(Conversation, portable_record_round_trip_and_strict_version) {
    auto record = empty();
    Encoded encoded;
    ASSERT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), record, encoded),
              AYNI_OK);
    ConversationRecord restored;
    ASSERT_EQ(
        Conversation::decode(::ayni::test::conversation_workspace(), encoded.view(), restored),
        AYNI_OK);
    EXPECT_EQ(restored.id, 11U);
    EXPECT_EQ(restored.owner_instance_id, 22U);
    const auto version = encoded.bytes[4];
    encoded.bytes[4] = version - 1; // Portable header: version at byte 4.
    EXPECT_EQ(
        Conversation::decode(::ayni::test::conversation_workspace(), encoded.view(), restored),
        AYNI_UNSUPPORTED_VERSION);
    encoded.bytes[4] = version;
    --encoded.size;
    EXPECT_EQ(
        Conversation::decode(::ayni::test::conversation_workspace(), encoded.view(), restored),
        AYNI_INVALID_ARGUMENT);
}
TEST(Conversation, new_turn_is_deterministic_and_retains_the_original) {
    const auto record = empty();
    Encoded a, b;
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  text("你好"), a),
              AYNI_OK);
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  text("你好"), b),
              AYNI_OK);
    EXPECT_EQ(a.size, b.size);
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(
        Conversation::validate_turn(
            const_cast<ayni_workspace_v1 *>(::ayni::test::conversation_workspace()), a.view()),
        AYNI_OK);
    ConversationRecord candidate;
    EXPECT_EQ(
        Conversation::finish(::ayni::test::conversation_workspace(), record, a.view(), candidate),
        AYNI_BUSY);
    EXPECT_EQ(record.revision, 0U);
}
// ADR-0096: input length is a token question for the ToolLoop; the parameters hold a reference.
TEST(Conversation, long_input_is_a_body_and_invalid_input_leaves_output_unchanged) {
    const auto record = empty();
    Encoded encoded;
    encoded.size = 71;
    EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  {"\xED\xA0\x80", 3}, encoded),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  {nullptr, 1}, encoded),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(encoded.size, 71U);
    Encoded small, large;
    const std::string long_text(200 * 1024, 'a');
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  text("a"), small),
              AYNI_OK);
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  {long_text.data(), static_cast<uint32_t>(long_text.size())},
                                  large),
              AYNI_OK);
    EXPECT_EQ(large.size, small.size);
    BodyRef input{};
    ASSERT_EQ(Conversation::input(::ayni::test::conversation_workspace(), large.view(), input),
              AYNI_OK);
    EXPECT_EQ(read(input), long_text);
}
TEST(Conversation, whole_delete_clears_text_and_lineage_together) {
    auto record = empty();
    record.revision = 4;
    record.last_round = 2;
    record.summary_first = 1;
    record.summary_last = 2;
    record.summary = body("inferred");
    ConversationRecord candidate;
    ASSERT_EQ(Conversation::erase(::ayni::test::conversation_workspace(), record, candidate),
              AYNI_OK);
    EXPECT_EQ(candidate.summary.size, 0U);
    EXPECT_EQ(candidate.summary_first, 0U);
    EXPECT_EQ(candidate.summary_last, 0U);
    EXPECT_EQ(candidate.last_round, 2U);
    EXPECT_EQ(candidate.generation, 2U);
    EXPECT_EQ(candidate.revision, 5U);
    EXPECT_EQ(read(record.summary), "inferred"); // Caller has not committed replacement yet.
}
TEST(Conversation, inconsistent_lineage_and_exhausted_generation_are_rejected) {
    auto record = empty();
    record.last_round = 1;
    Encoded encoded;
    EXPECT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), record, encoded),
              AYNI_INVALID_ARGUMENT);
    record = empty();
    record.generation = UINT64_MAX;
    ConversationRecord candidate;
    EXPECT_EQ(Conversation::erase(::ayni::test::conversation_workspace(), record, candidate),
              AYNI_CAPACITY_EXCEEDED);
}
TEST(BuiltinRules, invalid_and_duplicate_configuration_is_rejected) {
    Rules rules;
    RuleStep step{
        text("step"),
        1,
        {AYNI_POLICY_REQUEST_CALL, text("echo"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}}};
    Rule rule{text("routine"), 1, &step, 1};
    EXPECT_EQ(rules.configure({text("rules"), 1, nullptr, 0}, &rule, 0), AYNI_INVALID_ARGUMENT);
    step.version = 0;
    EXPECT_EQ(rules.configure({text("rules"), 1, nullptr, 0}, &rule, 1), AYNI_INVALID_ARGUMENT);
    step.version = 1;
    ASSERT_EQ(rules.configure({text("rules"), 1, nullptr, 0}, &rule, 1), AYNI_OK);
    EXPECT_EQ(rules.configure({text("rules"), 1, nullptr, 0}, &rule, 1), AYNI_INVALID_STATE);
}
} // namespace

namespace {
// Same snapshot twice produces identical request bytes and checkpoint bytes, and the turn's
// parameters hold references only, however long the retained texts are (ADR-0096).
TEST(BuiltinLoop, long_context_keeps_parameters_small_and_decisions_are_deterministic) {
    auto record = empty();
    record.revision = 5;
    record.last_round = 5;
    record.summary_first = 1;
    record.summary_last = 2;
    record.summary = body(std::string(4096, 's'));
    retain(record, 3, 2048);
    std::string user(4096, 'n');
    Encoded parameters;
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  {user.data(), static_cast<uint32_t>(user.size())}, parameters),
              AYNI_OK);
    EXPECT_LE(parameters.size, ToolLoop::checkpoint_bytes);
    ToolLoop loop;
    auto config = loop_config();
    config.summary.binding_id = text("summary");
    ASSERT_EQ(loop.configure(config, ::ayni::test::loop_workspace()), AYNI_OK);
    auto descriptor = loop.describe();
    ayni_task_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.owner_instance_id = record.owner_instance_id;
    // ADR-0085: this isolated snapshot has an empty authorized directory.
    snapshot.capability_context = [](const void *, const ayni_capability_context_v1 *,
                                     ayni_capability_context_v1 *current) {
        *current = {};
        current->boot_epoch = 1;
        current->owner_instance_id = 22;
        current->goal_type_version = 1;
        current->goal_type_size = 1;
        current->goal_type_id[0] = 'x';
        return AYNI_OK;
    };
    snapshot.goal_parameters = parameters.view();
    snapshot.call_result = [](const void *, ayni_string_v1, ayni_task_call_result_v1 *) {
        return AYNI_NOT_FOUND;
    };
    std::string requests[2];
    Encoded checkpoints[2];
    for (uint32_t at = 0; at < 2; ++at) {
        ayni_policy_output_v1 output{};
        output.struct_size = sizeof(output);
        output.checkpoint = checkpoints[at].bytes.data();
        output.checkpoint_capacity = ToolLoop::checkpoint_bytes;
        output.call_context = &requests[at];
        output.request_call = [](void *ctx, const ayni_call_request_v1 *request) {
            std::string body;
            if (!model_body(*request, body)) {
                return AYNI_INVALID_ARGUMENT;
            }
            *static_cast<std::string *>(ctx) =
                std::string(request->step_key.data, request->step_key.size) + body;
            return AYNI_OK;
        };
        ASSERT_EQ(descriptor.decide(descriptor.ctx, &snapshot, &output), AYNI_OK);
        checkpoints[at].size = output.checkpoint_size;
    }
    EXPECT_EQ(requests[0], requests[1]);
    EXPECT_EQ(checkpoints[0].bytes, checkpoints[1].bytes);
    // ADR-0085: history size alone no longer forces a fixed-round summary.
    EXPECT_EQ(requests[0].rfind("model.0", 0), 0U);
    EXPECT_NE(requests[0].find(ayni::model::system_instructions), std::string::npos);
    EXPECT_NE(requests[0].find(user), std::string::npos);
    std::cout << "ConversationRecord=" << sizeof(ConversationRecord)
              << " ToolLoop=" << sizeof(ToolLoop) << " turn_encoding=" << parameters.size
              << " bytes\n";
}
// ADR-0096: a record names its texts, so its encoding does not grow with the conversation and a
// decoded copy reads the same texts back from the store.
TEST(Conversation, record_size_is_constant_as_history_grows_and_reopens_the_same_texts) {
    std::array<uint32_t, 3> sizes{};
    const std::array<uint32_t, 3> lengths{16, 4096, 128 * 1024};
    for (uint32_t at = 0; at < lengths.size(); ++at) {
        auto record = empty();
        record.revision = 3;
        record.last_round = 3;
        record.history = body(std::string(lengths[at], 'h'));
        retain(record, 3, lengths[at]);
        Encoded encoded;
        ASSERT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), record, encoded),
                  AYNI_OK);
        sizes[at] = encoded.size;
        ConversationRecord reopened;
        ASSERT_EQ(
            Conversation::decode(::ayni::test::conversation_workspace(), encoded.view(), reopened),
            AYNI_OK);
        std::vector<Round> rounds;
        ASSERT_EQ(Conversation::rounds(bodies(), reopened, rounds), AYNI_OK);
        ASSERT_EQ(rounds.size(), 3U);
        EXPECT_EQ(rounds.back().number, 3U);
        EXPECT_EQ(rounds.back().answer, std::string(lengths[at], 'a'));
        EXPECT_EQ(read(reopened.history), std::string(lengths[at], 'h'));
    }
    EXPECT_EQ(sizes[0], sizes[1]);
    EXPECT_EQ(sizes[1], sizes[2]);
}
// ADR-0073/0074: the instructions of the model request state the local date, weekday and zone,
// so the model resolves relative dates itself, apart from the conversation data. An untrusted
// clock is marked, an unusable one states "unknown", and the same sample gives the same bytes.
TEST(BuiltinLoop, model_request_instructions_state_the_local_time) {
    struct FakeClock {
        ayni_utc_sample_v1 sample{sizeof(ayni_utc_sample_v1), AYNI_UTC_TRUSTED, 1793149500};
        static void now(void *context, ayni_utc_sample_v1 *out) {
            *out = static_cast<FakeClock *>(context)->sample; // 2026-10-28T01:05:00Z
        }
    } clock;
    struct Body {
        std::string instructions;
        std::string data;
        std::string bytes;
    };
    const auto request = [&](const char *zone, bool port = true) {
        ToolLoop loop;
        auto config = loop_config();
        config.model.text = text("prefix");
        config.summary = config.model;
        config.clock = {&clock, port ? FakeClock::now : nullptr, text(zone)};
        EXPECT_EQ(loop.configure(config, ::ayni::test::loop_workspace()), AYNI_OK);
        Encoded parameters;
        EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), empty(),
                                      text("明天提醒我"), parameters),
                  AYNI_OK);
        ayni_task_snapshot_v1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.owner_instance_id = empty().owner_instance_id;
        // ADR-0085: this isolated snapshot has an empty authorized directory.
        snapshot.capability_context = [](const void *, const ayni_capability_context_v1 *,
                                         ayni_capability_context_v1 *current) {
            *current = {};
            current->boot_epoch = 1;
            current->owner_instance_id = 22;
            current->goal_type_version = 1;
            current->goal_type_size = 1;
            current->goal_type_id[0] = 'x';
            return AYNI_OK;
        };
        snapshot.goal_parameters = parameters.view();
        snapshot.call_result = [](const void *, ayni_string_v1, ayni_task_call_result_v1 *) {
            return AYNI_NOT_FOUND;
        };
        Encoded checkpoint;
        Body body;
        ayni_policy_output_v1 output{};
        output.struct_size = sizeof(output);
        output.checkpoint = checkpoint.bytes.data();
        output.checkpoint_capacity = ToolLoop::checkpoint_bytes;
        output.call_context = &body.bytes;
        output.request_call = [](void *ctx, const ayni_call_request_v1 *r) {
            return model_body(*r, *static_cast<std::string *>(ctx)) ? AYNI_OK
                                                                    : AYNI_INVALID_ARGUMENT;
        };
        auto descriptor = loop.describe();
        EXPECT_EQ(descriptor.decide(descriptor.ctx, &snapshot, &output), AYNI_OK);
        ayni::model::Reader messages({body.bytes.data(), static_cast<uint32_t>(body.bytes.size())});
        ayni::model::Message message;
        while (messages.next(message)) {
            const std::string content(message.text.data, message.text.size);
            if (message.role == ayni::model::Role::system && body.instructions.empty()) {
                body.instructions = content;
            } else if (message.role == ayni::model::Role::context &&
                       content.find("Current local time:") != std::string::npos) {
                body.instructions += content;
            } else if (message.role == ayni::model::Role::user) {
                body.data = content;
            }
        }
        EXPECT_TRUE(messages.finished());
        return body;
    };
    const auto shanghai = request("CST-8");
    EXPECT_EQ(shanghai.instructions, "prefix\nCurrent local time: 2026-10-28 09:05, Wednesday.\n"
                                     "Time zone: CST-8");
    EXPECT_EQ(shanghai.data, "明天提醒我");
    EXPECT_EQ(request("CST-8").bytes, shanghai.bytes);
    EXPECT_EQ(request("EST5").instructions,
              "prefix\nCurrent local time: 2026-10-27 20:05, Tuesday.\nTime zone: EST5");
    EXPECT_EQ(request("").instructions, "prefix\nCurrent local time: unknown.");
    EXPECT_EQ(request("", false).instructions, "prefix");
    EXPECT_EQ(request("CST-8", false).instructions,
              "prefix\nCurrent local time: unknown.\nTime zone: CST-8");
    clock.sample.trust = AYNI_UTC_UNTRUSTED;
    EXPECT_EQ(request("CST-8").instructions,
              "prefix\nCurrent local time: 2026-10-28 09:05, Wednesday (device clock not "
              "synchronized).\nTime zone: CST-8");
    for (const auto trust : {AYNI_UTC_UNAVAILABLE, static_cast<ayni_utc_trust_t>(3)}) {
        clock.sample.trust = trust;
        EXPECT_EQ(request("CST-8").instructions,
                  "prefix\nCurrent local time: unknown.\nTime zone: CST-8");
    }
    ToolLoop loop;
    auto config = loop_config();
    config.clock = {&clock, FakeClock::now, text("Asia/Shanghai")};
    EXPECT_EQ(loop.configure(config, ::ayni::test::loop_workspace()), AYNI_INVALID_ARGUMENT);
}
TEST(BuiltinLoop, foreign_conversation_owner_is_rejected_before_any_request) {
    Encoded parameters;
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), empty(),
                                  text("hello"), parameters),
              AYNI_OK);
    ToolLoop loop;
    const auto config = loop_config();
    ASSERT_EQ(loop.configure(config, ::ayni::test::loop_workspace()), AYNI_OK);
    auto descriptor = loop.describe();
    ayni_task_snapshot_v1 snapshot{};
    ayni_policy_output_v1 output{};
    snapshot.goal_parameters = parameters.view();
    snapshot.owner_instance_id = 23;
    // ADR-0085: this isolated snapshot has an empty authorized directory.
    snapshot.capability_context = [](const void *, const ayni_capability_context_v1 *,
                                     ayni_capability_context_v1 *current) {
        *current = {};
        current->boot_epoch = 1;
        current->owner_instance_id = 22;
        current->goal_type_version = 1;
        current->goal_type_size = 1;
        current->goal_type_id[0] = 'x';
        return AYNI_OK;
    };
    EXPECT_EQ(descriptor.decide(descriptor.ctx, &snapshot, &output), AYNI_DENIED);
}
} // namespace

namespace {
// ADR-0056: byte input and begin both reject a known exhausted turn sequence
// before any model request. Construct raw parameters using the public record codec.
TEST(Conversation, encoded_turn_with_exhausted_round_numbers_is_rejected) {
    auto record = empty();
    record.last_round = UINT64_MAX;
    record.summary_first = 1;
    record.summary_last = UINT64_MAX;
    record.summary = body("s");
    Encoded record_bytes, seed;
    ASSERT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), record, record_bytes),
              AYNI_OK);
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), empty(),
                                  text("x"), seed),
              AYNI_OK);
    constexpr uint32_t record_header_bytes = 8; // AYNC + version.
    constexpr uint32_t turn_header_bytes = 28; // AYNT + version + done + base_revision + memory_id.
    Encoded empty_record;
    ASSERT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), empty(), empty_record),
              AYNI_OK);
    const uint32_t seed_tail =
        seed.size - turn_header_bytes - (empty_record.size - record_header_bytes);
    Encoded raw;
    std::memcpy(raw.bytes.data(), seed.bytes.data(), turn_header_bytes);
    const auto record_body = record_bytes.size - record_header_bytes;
    std::memcpy(raw.bytes.data() + turn_header_bytes,
                record_bytes.bytes.data() + record_header_bytes, record_body);
    std::memcpy(raw.bytes.data() + turn_header_bytes + record_body,
                seed.bytes.data() + seed.size - seed_tail, seed_tail);
    raw.size = turn_header_bytes + record_body + seed_tail;
    EXPECT_EQ(
        Conversation::validate_turn(
            const_cast<ayni_workspace_v1 *>(::ayni::test::conversation_workspace()), raw.view()),
        AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), record,
                                  text("x"), seed),
              AYNI_CAPACITY_EXCEEDED);
}
} // namespace

namespace {
// ADR-0060: an independent accepted input survives invalidation and orderly record round-trip.
TEST(Conversation, unpublished_input_survives_derived_erasure_and_joins_the_next_turn) {
    auto record = empty();
    record.draft = body("first");
    Encoded stored, parameters;
    ASSERT_EQ(Conversation::encode(::ayni::test::conversation_workspace(), record, stored),
              AYNI_OK);
    ConversationRecord restored, erased;
    ASSERT_EQ(Conversation::decode(::ayni::test::conversation_workspace(), stored.view(), restored),
              AYNI_OK);
    ASSERT_EQ(Conversation::erase_derived(::ayni::test::conversation_workspace(), bodies(),
                                          restored, erased),
              AYNI_OK);
    EXPECT_EQ(read(erased.draft), "first");
    ASSERT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), erased,
                                  text("next"), parameters),
              AYNI_OK);
    BodyRef input{};
    ASSERT_EQ(Conversation::input(::ayni::test::conversation_workspace(), parameters.view(), input),
              AYNI_OK);
    EXPECT_EQ(read(input), "first\nnext");
    ASSERT_EQ(Conversation::erase(::ayni::test::conversation_workspace(), erased, restored),
              AYNI_OK);
    EXPECT_EQ(restored.draft.id, 0U);
}
TEST(Conversation, superseded_publication_keeps_original_turn_identity) {
    auto record = empty();
    record.draft = body("u");
    ConversationRecord retained, erased;
    ASSERT_EQ(Conversation::retain_published_input(::ayni::test::conversation_workspace(), bodies(),
                                                   record, 1, retained),
              AYNI_OK);
    std::vector<Round> rounds;
    ASSERT_EQ(Conversation::rounds(bodies(), retained, rounds), AYNI_OK);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].number, 1U);
    EXPECT_EQ(rounds[0].user, "u");
    ASSERT_EQ(Conversation::erase_round(::ayni::test::conversation_workspace(), bodies(), retained,
                                        1, erased),
              AYNI_OK);
    EXPECT_EQ(erased.round_count, 0U);
    EXPECT_EQ(erased.draft.id, 0U);
    for (uint64_t turn = 2; turn <= 6; ++turn) {
        retained.draft = record.draft;
        ASSERT_EQ(Conversation::retain_published_input(::ayni::test::conversation_workspace(),
                                                       bodies(), retained, turn, erased),
                  AYNI_OK);
        retained = erased;
    }
    EXPECT_EQ(retained.round_count, 6U);
    Encoded encoded;
    EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), retained,
                                  text("next"), encoded),
              AYNI_OK);
    ASSERT_EQ(Conversation::erase_round(::ayni::test::conversation_workspace(), bodies(), retained,
                                        1, erased),
              AYNI_OK);
    EXPECT_EQ(erased.round_count, 5U);
    EXPECT_EQ(Conversation::begin(::ayni::test::conversation_workspace(), bodies(), erased,
                                  text("next"), encoded),
              AYNI_OK);
}
} // namespace

// ADR-0090 / C-SDK-007: codecs admit scratch before publishing parameters;
// the GoalType validator borrows its explicit descriptor as callback context.
TEST(Conversation, short_workspace_preserves_parameters_and_valid_region_supports_callback) {
    ayni::Workspace memory;
    ASSERT_EQ(memory.create(Conversation::workspace_requirements), AYNI_OK);
    Encoded parameters;
    parameters.size = 7;
    parameters.bytes.front() = 91;
    auto short_region = *memory.get();
    --short_region.byte_capacity;
    EXPECT_EQ(Conversation::begin(&short_region, bodies(), empty(), text("hello"), parameters),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(parameters.size, 7U);
    EXPECT_EQ(parameters.bytes.front(), 91U);
    ASSERT_EQ(Conversation::begin(memory.get(), bodies(), empty(), text("hello"), parameters),
              AYNI_OK);
    EXPECT_EQ(Conversation::validate_turn(nullptr, parameters.view()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(Conversation::validate_turn(const_cast<ayni_workspace_v1 *>(memory.get()),
                                          parameters.view()),
              AYNI_OK);
}

// ADR-0090: strategy allocation belongs to the caller before registration/dispatch.
TEST(ToolLoop, short_workspace_refuses_configuration_without_consuming_the_instance) {
    ayni::Workspace memory;
    ASSERT_EQ(memory.create(ToolLoop::workspace_requirements), AYNI_OK);
    auto config = loop_config();
    config.identity.id = text("bounded.loop");
    ToolLoop loop;
    auto short_region = *memory.get();
    --short_region.byte_capacity;
    EXPECT_EQ(loop.configure(config, &short_region), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(loop.configure(config, memory.get()), AYNI_OK);
    EXPECT_EQ(loop.configure(config, memory.get()), AYNI_INVALID_STATE);
}
