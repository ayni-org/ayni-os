// ADR-0056: caller-owned, portable values. No external persistence is implied.
#include <ayni/strategies/builtin.h>
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
TEST(Conversation, portable_record_round_trip_and_strict_version) {
    auto record = empty();
    Encoded encoded;
    ASSERT_EQ(Conversation::encode(record, encoded), AYNI_OK);
    ConversationRecord restored;
    ASSERT_EQ(Conversation::decode(encoded.view(), restored), AYNI_OK);
    EXPECT_EQ(restored.id, 11U);
    EXPECT_EQ(restored.owner_instance_id, 22U);
    encoded.bytes[4] = 3; // Portable header: version at byte 4.
    EXPECT_EQ(Conversation::decode(encoded.view(), restored), AYNI_UNSUPPORTED_VERSION);
    encoded.bytes[4] = 2;
    --encoded.size;
    EXPECT_EQ(Conversation::decode(encoded.view(), restored), AYNI_INVALID_ARGUMENT);
}
TEST(Conversation, new_turn_is_deterministic_and_retains_the_original) {
    const auto record = empty();
    Encoded a, b;
    ASSERT_EQ(Conversation::begin(record, text("你好"), a), AYNI_OK);
    ASSERT_EQ(Conversation::begin(record, text("你好"), b), AYNI_OK);
    EXPECT_EQ(a.size, b.size);
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(Conversation::validate_turn(nullptr, a.view()), AYNI_OK);
    ConversationRecord candidate;
    EXPECT_EQ(Conversation::finish(record, a.view(), candidate), AYNI_BUSY);
    EXPECT_EQ(record.revision, 0U);
}
TEST(Conversation, bounded_input_rejects_invalid_utf8_and_oversize_without_changing_output) {
    const auto record = empty();
    Encoded encoded;
    encoded.size = 71;
    const std::string long_text(turn_text_max + 1U, 'a');
    EXPECT_EQ(Conversation::begin(
                  record, {long_text.data(), static_cast<uint32_t>(long_text.size())}, encoded),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(Conversation::begin(record, {"\xED\xA0\x80", 3}, encoded), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(Conversation::begin(record, {nullptr, 1}, encoded), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(encoded.size, 71U);
}
TEST(Conversation, whole_delete_clears_text_and_lineage_together) {
    auto record = empty();
    record.revision = 4;
    record.last_round = 2;
    record.summary_first = 1;
    record.summary_last = 2;
    std::memcpy(record.summary.bytes.data(), "inferred", 8);
    record.summary.size = 8;
    ConversationRecord candidate;
    ASSERT_EQ(Conversation::erase(record, candidate), AYNI_OK);
    EXPECT_EQ(candidate.summary.size, 0U);
    EXPECT_EQ(candidate.summary_first, 0U);
    EXPECT_EQ(candidate.summary_last, 0U);
    EXPECT_EQ(candidate.last_round, 2U);
    EXPECT_EQ(candidate.generation, 2U);
    EXPECT_EQ(candidate.revision, 5U);
    EXPECT_EQ(record.summary.size, 8U); // Caller has not committed replacement yet.
}
TEST(Conversation, inconsistent_lineage_and_exhausted_generation_are_rejected) {
    auto record = empty();
    record.last_round = 1;
    Encoded encoded;
    EXPECT_EQ(Conversation::encode(record, encoded), AYNI_INVALID_ARGUMENT);
    record = empty();
    record.generation = UINT64_MAX;
    ConversationRecord candidate;
    EXPECT_EQ(Conversation::erase(record, candidate), AYNI_CAPACITY_EXCEEDED);
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
// Same snapshot twice produces identical request bytes and checkpoint bytes;
// also exercises the largest supported recent-history parameter representation.
TEST(BuiltinLoop, maximum_context_is_bounded_and_decisions_are_deterministic) {
    auto record = empty();
    record.revision = 5;
    record.last_round = 5;
    record.summary_first = 1;
    record.summary_last = 2;
    record.summary.size = summary_text_max;
    record.summary.bytes.fill('s');
    record.round_count = recent_rounds_max;
    for (uint32_t at = 0; at < recent_rounds_max; ++at) {
        auto &round = record.rounds[at];
        round.number = at + 3U;
        round.user.size = turn_text_max;
        round.user.bytes.fill('u');
        round.answer.size = turn_text_max;
        round.answer.bytes.fill('a');
    }
    std::string user(turn_text_max, 'n');
    Encoded parameters;
    ASSERT_EQ(Conversation::begin(record, {user.data(), turn_text_max}, parameters), AYNI_OK);
    EXPECT_LE(parameters.size, state_bytes_max);
    ToolLoop loop;
    ToolLoopConfig config;
    config.identity.id = text("loop");
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, text("model"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    config.summary.binding_id = text("summary");
    ASSERT_EQ(loop.configure(config), AYNI_OK);
    auto descriptor = loop.describe();
    ayni_task_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.owner_instance_id = record.owner_instance_id;
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
        output.checkpoint_capacity = state_bytes_max;
        output.call_context = &requests[at];
        output.request_call = [](void *ctx, const ayni_call_request_v1 *request) {
            *static_cast<std::string *>(ctx) =
                std::string(request->step_key.data, request->step_key.size) +
                std::string(request->text.data, request->text.size);
            return AYNI_OK;
        };
        ASSERT_EQ(descriptor.decide(descriptor.ctx, &snapshot, &output), AYNI_OK);
        checkpoints[at].size = output.checkpoint_size;
    }
    EXPECT_EQ(requests[0], requests[1]);
    EXPECT_EQ(checkpoints[0].bytes, checkpoints[1].bytes);
    EXPECT_NE(requests[0].find("dialog.summary.v1"), std::string::npos);
    std::cout << "ConversationRecord=" << sizeof(ConversationRecord)
              << " ToolLoop=" << sizeof(ToolLoop) << " max_turn_encoding=" << parameters.size
              << " bytes\n";
}
// ADR-0073: the model request states the local date, weekday and zone, so the model resolves
// relative dates itself; an untrusted clock is marked, and a missing one states "unknown".
TEST(BuiltinLoop, model_request_states_the_local_time) {
    struct FakeClock {
        ayni_utc_sample_v1 sample{sizeof(ayni_utc_sample_v1), AYNI_UTC_TRUSTED, 1793149500};
        static void now(void *context, ayni_utc_sample_v1 *out) {
            *out = static_cast<FakeClock *>(context)->sample; // 2026-10-28T01:05:00Z
        }
    } clock;
    const auto request = [&](const char *zone) {
        ToolLoop loop;
        ToolLoopConfig config;
        config.identity.id = text("loop");
        config.model = {AYNI_POLICY_REQUEST_MODEL,
                        text("model"),
                        1,
                        {sizeof(ayni_scope_v1), 0, nullptr},
                        text("prefix")};
        config.summary = config.model;
        config.clock = {&clock, FakeClock::now, text(zone)};
        EXPECT_EQ(loop.configure(config), AYNI_OK);
        Encoded parameters;
        EXPECT_EQ(Conversation::begin(empty(), text("明天提醒我"), parameters), AYNI_OK);
        ayni_task_snapshot_v1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.owner_instance_id = empty().owner_instance_id;
        snapshot.goal_parameters = parameters.view();
        snapshot.call_result = [](const void *, ayni_string_v1, ayni_task_call_result_v1 *) {
            return AYNI_NOT_FOUND;
        };
        Encoded checkpoint;
        std::string sent;
        ayni_policy_output_v1 output{};
        output.struct_size = sizeof(output);
        output.checkpoint = checkpoint.bytes.data();
        output.checkpoint_capacity = state_bytes_max;
        output.call_context = &sent;
        output.request_call = [](void *ctx, const ayni_call_request_v1 *r) {
            *static_cast<std::string *>(ctx) = std::string(r->text.data, r->text.size);
            return AYNI_OK;
        };
        auto descriptor = loop.describe();
        EXPECT_EQ(descriptor.decide(descriptor.ctx, &snapshot, &output), AYNI_OK);
        return sent;
    };
    EXPECT_EQ(request("CST-8").rfind("prefix\nCurrent local time: 2026-10-28 09:05, Wednesday, "
                                     "time zone CST-8.",
                                     0),
              0U);
    EXPECT_NE(
        request("EST5").find("Current local time: 2026-10-27 20:05, Tuesday, time zone EST5."),
        std::string::npos);
    clock.sample.trust = AYNI_UTC_UNTRUSTED;
    EXPECT_NE(request("CST-8").find("Wednesday, time zone CST-8 (device clock not synchronized)."),
              std::string::npos);
    clock.sample.trust = AYNI_UTC_UNAVAILABLE;
    EXPECT_NE(request("CST-8").find("Current local time: unknown."), std::string::npos);
    ToolLoop loop;
    ToolLoopConfig config;
    config.identity.id = text("loop");
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, text("model"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    config.clock = {&clock, FakeClock::now, text("Asia/Shanghai")};
    EXPECT_EQ(loop.configure(config), AYNI_INVALID_ARGUMENT);
}
TEST(BuiltinLoop, foreign_conversation_owner_is_rejected_before_any_request) {
    Encoded parameters;
    ASSERT_EQ(Conversation::begin(empty(), text("hello"), parameters), AYNI_OK);
    ToolLoop loop;
    ToolLoopConfig config;
    config.identity.id = text("loop");
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, text("model"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    ASSERT_EQ(loop.configure(config), AYNI_OK);
    auto descriptor = loop.describe();
    ayni_task_snapshot_v1 snapshot{};
    ayni_policy_output_v1 output{};
    snapshot.goal_parameters = parameters.view();
    snapshot.owner_instance_id = 23;
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
    record.summary.size = 1;
    record.summary.bytes[0] = 's';
    Encoded record_bytes, seed;
    ASSERT_EQ(Conversation::encode(record, record_bytes), AYNI_OK);
    ASSERT_EQ(Conversation::begin(empty(), text("x"), seed), AYNI_OK);
    constexpr uint32_t record_header_bytes = 8; // AYNC + version.
    constexpr uint32_t turn_header_bytes = 28; // AYNT + version + done + base_revision + memory_id.
    constexpr uint32_t input_bytes = 5;        // u32 size and one UTF-8 byte.
    Encoded raw;
    std::memcpy(raw.bytes.data(), seed.bytes.data(), turn_header_bytes);
    const auto record_body = record_bytes.size - record_header_bytes;
    std::memcpy(raw.bytes.data() + turn_header_bytes,
                record_bytes.bytes.data() + record_header_bytes, record_body);
    std::memcpy(raw.bytes.data() + turn_header_bytes + record_body,
                seed.bytes.data() + seed.size - input_bytes, input_bytes);
    raw.size = turn_header_bytes + record_body + input_bytes;
    EXPECT_EQ(Conversation::validate_turn(nullptr, raw.view()), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(Conversation::begin(record, text("x"), seed), AYNI_CAPACITY_EXCEEDED);
}
} // namespace

namespace {
// ADR-0060: an independent accepted input survives invalidation and orderly record round-trip.
TEST(Conversation, unpublished_input_survives_derived_erasure_and_is_bounded_when_resumed) {
    auto record = empty();
    record.draft.size = 5;
    std::memcpy(record.draft.bytes.data(), "first", record.draft.size);
    Encoded stored, parameters;
    ASSERT_EQ(Conversation::encode(record, stored), AYNI_OK);
    ConversationRecord restored, erased;
    ASSERT_EQ(Conversation::decode(stored.view(), restored), AYNI_OK);
    ASSERT_EQ(Conversation::erase_derived(restored, erased), AYNI_OK);
    EXPECT_EQ(erased.draft.size, 5U);
    ASSERT_EQ(Conversation::begin(erased, text("next"), parameters), AYNI_OK);
    Text<turn_text_max> input;
    ASSERT_EQ(Conversation::input(parameters.view(), input), AYNI_OK);
    EXPECT_EQ(std::string(input.bytes.data(), input.size), "first\nnext");
    erased.draft.size = turn_text_max;
    erased.draft.bytes.fill('u');
    EXPECT_EQ(Conversation::begin(erased, text("x"), parameters), AYNI_CAPACITY_EXCEEDED);
    ASSERT_EQ(Conversation::erase(erased, restored), AYNI_OK);
    EXPECT_EQ(restored.draft.size, 0U);
}
TEST(Conversation, superseded_publication_keeps_original_turn_identity_and_reserves_capacity) {
    auto record = empty();
    record.draft.size = 1;
    record.draft.bytes[0] = 'u';
    ConversationRecord retained, erased;
    ASSERT_EQ(Conversation::retain_published_input(record, 1, retained), AYNI_OK);
    ASSERT_EQ(retained.round_count, 1U);
    EXPECT_EQ(retained.rounds[0].number, 1U);
    ASSERT_EQ(Conversation::erase_round(retained, 1, erased), AYNI_OK);
    EXPECT_EQ(erased.round_count, 0U);
    EXPECT_EQ(erased.draft.size, 0U);
    for (uint64_t turn = 2; turn <= retained_rounds_max; ++turn) {
        retained.draft = record.draft;
        ASSERT_EQ(Conversation::retain_published_input(retained, turn, erased), AYNI_OK);
        retained = erased;
    }
    Encoded encoded;
    EXPECT_EQ(Conversation::begin(retained, text("next"), encoded), AYNI_CAPACITY_EXCEEDED);
    ASSERT_EQ(Conversation::erase_round(retained, 1, erased), AYNI_OK);
    EXPECT_EQ(Conversation::begin(erased, text("next"), encoded), AYNI_OK);
}
} // namespace
