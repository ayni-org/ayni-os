#include "configuration.h"
#include "reply_voice.h"
#include "speech_profile.h"
#include <ayni/authority.h>
#include <ayni/context.h>
#include <ayni/host_authority.h>
#include <ayni/hosts/sim.h>
#include <ayni/values.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
namespace {
using ayni::initialized;
ayni_string_v1 text(const char *value) {
    return {value, uint32_t(std::strlen(value))};
}
class ReplyVoiceKernel : public testing::Test {
  protected:
    struct Bodies {
        uint64_t next = 0;
        uint32_t write_waits = 0;
        std::map<uint64_t, std::string> values;
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> identities;
        static ayni_status_t write(void *context, uint64_t scope, ayni_bytes_v1 bytes,
                                   ayni_body_ref_v1 *out) {
            auto &self = *static_cast<Bodies *>(context);
            if (self.write_waits) {
                --self.write_waits;
                return AYNI_NOT_READY;
            }
            const auto identity = std::make_pair(scope, ayni_body_digest(bytes));
            const auto found = self.identities.find(identity);
            const auto id = found == self.identities.end() ? ++self.next : found->second;
            self.identities[identity] = id;
            self.values[id].assign(reinterpret_cast<const char *>(bytes.data), bytes.size);
            *out = {id, ayni_body_digest(bytes), bytes.size, 0};
            return AYNI_OK;
        }
        static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                                  uint32_t size) {
            const auto &values = static_cast<Bodies *>(context)->values;
            const auto found = values.find(ref->id);
            if (found == values.end() || size < found->second.size()) {
                return AYNI_NOT_FOUND;
            }
            std::memcpy(out, found->second.data(), found->second.size());
            return AYNI_OK;
        }
        static void release(void *context, const ayni_body_ref_v1 *ref) {
            auto &self = *static_cast<Bodies *>(context);
            self.values.erase(ref->id);
            for (auto at = self.identities.begin(); at != self.identities.end();) {
                if (at->second == ref->id) {
                    at = self.identities.erase(at);
                } else {
                    ++at;
                }
            }
        }
    } bodies;
    ayni_body_store_v1 body_port{sizeof(body_port), 0, &bodies, Bodies::write, Bodies::read,
                                 Bodies::release};
    ayni::hosts::sim::Host host;
    ayni::Kernel kernel;
    wifichat::ReplyVoice speech;
    ayni::CommandClient commands;
    uint64_t session = 0, owner = 0, context_id = 0;
    uint32_t calls = 0, cancellations = 0;
    bool hold_audio = false, delayed_cancel = false;
    ayni_provider_completion_t *pending_audio = nullptr;
    ayni_attempt_ref_v1 audio_attempt{};
    std::string heard;
    std::array<ayni_schema_node_v1, 2> input{};
    ayni_schema_node_v1 output{};
    ayni_business_contract_v1 business{};
    ayni_call_capability_v1 offer{};
    static ayni_submit_result_t speak(void *context, const ayni_provider_request_v1 *request,
                                      ayni_provider_completion_t *completion) {
        auto &self = *static_cast<ReplyVoiceKernel *>(context);
        ayni::ValueReader reader;
        EXPECT_EQ(reader.read(request->value), AYNI_OK);
        const auto *value = reader.field(1);
        if (!value || value->kind != AYNI_VALUE_TEXT) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        EXPECT_LE(value->value.text.size, wifichat::SpeechProfile::text_bytes);
        EXPECT_EQ(ayni_validate_utf8(value->value.text), AYNI_OK);
        self.heard.append(value->value.text.data, value->value.text.size);
        ++self.calls;
        if (self.hold_audio) {
            self.pending_audio = completion;
            self.audio_attempt = request->attempt;
            return AYNI_SUBMIT_ACCEPTED;
        }
        auto result = initialized<ayni_provider_result_v1>();
        result.attempt = request->attempt;
        result.outcome = AYNI_CALL_COMPLETED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        result.result = text("played");
        EXPECT_EQ(ayni_provider_post(completion, &result), AYNI_OK);
        return AYNI_SUBMIT_ACCEPTED;
    }
    static ayni_submit_result_t cancel_audio(void *context, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<ReplyVoiceKernel *>(context);
        if (!self.pending_audio || attempt.request_id != self.audio_attempt.request_id) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        ++self.cancellations;
        if (!self.delayed_cancel) {
            self.finish_cancel();
        }
        return AYNI_SUBMIT_ACCEPTED;
    }
    void finish_cancel() {
        auto result = initialized<ayni_provider_result_v1>();
        result.attempt = audio_attempt;
        result.outcome = AYNI_CALL_CANCELLED;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_PARTIAL;
        EXPECT_EQ(ayni_provider_post(pending_audio, &result), AYNI_OK);
        pending_audio = nullptr;
    }
    void SetUp() override {
        const auto config =
            wifichat::chat_config(AYNI_CALL_INPUT_BYTES_DEFAULT, wifichat::summary_bytes);
        const ayni::Quiesce quiesce{
            &host, [](void *value) -> ayni_status_t {
                return static_cast<ayni::hosts::sim::Host *>(value)->pending_storage() ? AYNI_BUSY
                                                                                       : AYNI_OK;
            }};
        ASSERT_EQ(kernel.create(config, host.table(), quiesce), AYNI_OK);
        const ayni_principal_desc_v1 principal{sizeof(principal), AYNI_PRINCIPAL_OWNER, 1, 1, 0};
        const ayni_agent_desc_v1 agent{sizeof(agent), 0, 1};
        ASSERT_EQ(ayni_register_principal(kernel.get(), &principal), AYNI_OK);
        ASSERT_EQ(ayni_register_agent(kernel.get(), &agent), AYNI_OK);
        const ayni_capability_desc_v1 capability{
            sizeof(capability), 2, text("speaker.speech"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel.get(), &capability), AYNI_OK);
        input[0].kind = AYNI_VALUE_OBJECT;
        input[0].subtree_end = uint32_t(input.size());
        input[1].kind = AYNI_VALUE_TEXT;
        input[1].field_id = 1;
        input[1].subtree_end = uint32_t(input.size());
        input[1].name = text("text");
        input[1].flags = AYNI_SCHEMA_REQUIRED;
        input[1].min_count = 1;
        input[1].max_count = wifichat::SpeechProfile::text_bytes;
        output.kind = AYNI_VALUE_TEXT;
        output.subtree_end = 1;
        output.max_count = 64;
        business = {{input.data(), uint32_t(input.size())}, {&output, 1}, 0, 0, nullptr};
        offer = {sizeof(offer),
                 2,
                 text("speaker.speech"),
                 {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                  AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, 1000,
                  AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 2048, 64, 0, nullptr, nullptr},
                 &business};
        ASSERT_EQ(ayni_register_call_capability(kernel.get(), &offer), AYNI_OK);
        const ayni_provider_v1 provider{sizeof(provider),
                                        1,
                                        text("test.speaker"),
                                        text("test.endpoint"),
                                        {},
                                        this,
                                        1,
                                        0,
                                        &offer,
                                        speak,
                                        cancel_audio,
                                        nullptr,
                                        nullptr};
        ASSERT_EQ(ayni_register_provider(kernel.get(), &provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      2,
                                      text("speaker.speak_text"),
                                      text("speaker.speech"),
                                      2,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        ASSERT_EQ(ayni_register_binding(kernel.get(), &binding), AYNI_OK);
        const auto policy = speech.strategy(&body_port);
        const auto type = wifichat::ReplyVoice::goal_type();
        ASSERT_EQ(ayni_register_strategy(kernel.get(), &policy), AYNI_OK);
        ASSERT_EQ(ayni_register_goal_type(kernel.get(), &type), AYNI_OK);
        ASSERT_EQ(kernel.start(), AYNI_OK);
        const ayni_session_desc_v1 session_desc{sizeof(session_desc), AYNI_CHANNEL_MANAGEMENT, 1, 1,
                                                0};
        ASSERT_TRUE(until(
            [&] { return ayni_open_session(kernel.get(), &session_desc, &session) == AYNI_OK; }));
        auto info = initialized<ayni_agent_info_v1>();
        ASSERT_EQ(ayni_get_agent(kernel.get(), 1, &info), AYNI_OK);
        owner = info.instance_id;
        auto state = initialized<ayni_kernel_info_v1>();
        ASSERT_EQ(ayni_get_kernel_info(kernel.get(), &state), AYNI_OK);
        ASSERT_EQ(commands.configure(kernel.get(), 1, session, state.boot_epoch), AYNI_OK);
        auto opened = initialized<ayni_context_command_v1>();
        opened.owner_instance_id = owner;
        ASSERT_EQ(commands.submit(AYNI_INTENT_CONTEXT_OPEN, &opened).status, AYNI_OK);
        ASSERT_TRUE(until([&] { return !commands.pending(); }));
        auto anchor = initialized<ayni_context_info_v1>();
        ASSERT_EQ(ayni_get_context(kernel.get(), session, owner, &anchor), AYNI_OK);
        context_id = anchor.context_id;
        speech.set_accepting(true);
    }
    void grant() {
        const ayni_grant_create_v1 body{sizeof(body),
                                        2,
                                        text("speaker.speech"),
                                        owner,
                                        0,
                                        AYNI_GRANT_PERMANENT,
                                        0,
                                        0,
                                        {sizeof(ayni_scope_v1), 0, nullptr}};
        ASSERT_EQ(commands.submit(AYNI_INTENT_GRANT_CREATE, &body).status, AYNI_OK);
        ASSERT_TRUE(until([&] { return !commands.pending(); }));
    }
    template <class Done> bool until(Done done) {
        constexpr uint32_t steps = 2000;
        for (uint32_t at = 0; at < steps; ++at) {
            if (done()) {
                return true;
            }
            host.advance_us(10000);
            host.complete_storage();
            auto report = initialized<ayni_step_report_v1>();
            if (kernel.step(8, 10000, report) != AYNI_OK || commands.poll() != AYNI_OK) {
                return false;
            }
        }
        return done();
    }
    ayni_status_t start(std::string answer) {
        auto source = initialized<ayni_context_info_v1>();
        const auto queried = ayni_get_context(kernel.get(), session, owner, &source);
        return queried == AYNI_OK ? speech.begin(std::move(answer), source) : queried;
    }
    ayni_status_t advance() {
        auto info = initialized<ayni_kernel_info_v1>();
        if (ayni_get_kernel_info(kernel.get(), &info) != AYNI_OK) {
            return AYNI_INTERNAL;
        }
        return speech.advance(kernel.get(), commands, session, owner, info.durable_seq);
    }
    void TearDown() override {
        const ayni::Progress progress{&host, [](void *context) -> ayni_status_t {
                                          auto &value =
                                              *static_cast<ayni::hosts::sim::Host *>(context);
                                          value.advance_us(10000);
                                          value.complete_storage();
                                          return AYNI_OK;
                                      }};
        EXPECT_EQ(kernel.drain(2000, progress), AYNI_OK);
        EXPECT_EQ(kernel.close(), AYNI_OK);
        speech.reset();
    }
};
// ADR-0096/0097: full UTF-8 replies use body-reference Goals and the existing authorized call.
TEST_F(ReplyVoiceKernel, CompleteLongAnswerUsesBoundedUtf8ChunksWithoutModelRequests) {
    grant();
    std::string answer;
    for (uint32_t at = 0; at < 600; ++at) {
        answer += "测试";
    }
    ASSERT_EQ(start(answer), AYNI_OK);
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(heard, answer);
    EXPECT_EQ(calls, 4U);
    EXPECT_TRUE(bodies.values.empty());
}
// No Grant is an ordinary admission outcome, not a storage/transport fault campaign.
TEST_F(ReplyVoiceKernel, MissingAuthorizationEndsVoiceWithoutBlockingTheSavedReply) {
    ASSERT_EQ(start("需要授权的语音"), AYNI_OK);
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_DENIED);
    EXPECT_EQ(calls, 0U);
    EXPECT_TRUE(bodies.values.empty());
}
// Switching off after one completed chunk prevents the next segment; no past chunk is replayed.
TEST_F(ReplyVoiceKernel, DisableStopsRemainingChunksAndKeepsTheSourceText) {
    grant();
    const std::string answer(2400, 'a');
    ASSERT_EQ(start(answer), AYNI_OK);
    ASSERT_TRUE(until([&] {
        if (calls) {
            return true;
        }
        EXPECT_EQ(advance(), AYNI_BUSY);
        return false;
    }));
    speech.cancel();
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(heard, answer.substr(0, 1024));
    EXPECT_TRUE(bodies.values.empty());
}
// Explicit switch-off cancels a live Provider borrow and waits for Core's durable termination.
TEST_F(ReplyVoiceKernel, DisableDuringPlaybackCancelsExactlyOnceAndDoesNotSpeakNextChunk) {
    grant();
    hold_audio = true;
    ASSERT_EQ(start(std::string(2400, 'b')), AYNI_OK);
    ASSERT_TRUE(until([&] {
        if (calls) {
            return true;
        }
        EXPECT_EQ(advance(), AYNI_BUSY);
        return false;
    }));
    speech.cancel();
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_EQ(pending_audio, nullptr);
    EXPECT_TRUE(bodies.values.empty());
}
// FileBodies deduplicates within a scope; automatic speech must own a different scope.
TEST_F(ReplyVoiceKernel, IdenticalRetainedConversationTextSurvivesSpeechCleanup) {
    grant();
    const std::string answer = "保留的相同正文";
    ayni_body_ref_v1 retained{};
    ASSERT_EQ(
        Bodies::write(&bodies, context_id,
                      {reinterpret_cast<const uint8_t *>(answer.data()), uint32_t(answer.size())},
                      &retained),
        AYNI_OK);
    ASSERT_EQ(start(answer), AYNI_OK);
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(heard, answer);
    ASSERT_EQ(bodies.values.size(), 1U);
    EXPECT_EQ(bodies.values.at(retained.id), answer);
}
// A normal context deletion invalidates derived speech, including an already accepted chunk.
TEST_F(ReplyVoiceKernel, ContextDeletionCancelsDerivedSpeechBeforeFurtherSegments) {
    grant();
    hold_audio = true;
    ASSERT_EQ(start(std::string(2400, 'c')), AYNI_OK);
    ASSERT_TRUE(until([&] {
        if (calls && !commands.pending()) {
            return true;
        }
        EXPECT_EQ(advance(), AYNI_BUSY);
        return false;
    }));
    auto context = initialized<ayni_context_info_v1>();
    ASSERT_EQ(ayni_get_context(kernel.get(), session, owner, &context), AYNI_OK);
    auto forget = initialized<ayni_context_command_v1>();
    forget.context_id = context.context_id;
    forget.expected_generation = context.generation;
    forget.action = AYNI_CONTEXT_ERASE_ALL;
    ASSERT_EQ(commands.submit(AYNI_INTENT_CONTEXT_FORGET, &forget, context.revision).status,
              AYNI_OK);
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_NE(result, AYNI_OK);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_EQ(pending_audio, nullptr);
}
// Normal asynchronous body preparation must drain on switch-off before ownership is released.
TEST_F(ReplyVoiceKernel, DisableDuringBodyPreparationLeavesNoSpeechOrPrivateChunk) {
    grant();
    bodies.write_waits = 2;
    ASSERT_EQ(start("异步保存中的语音"), AYNI_OK);
    EXPECT_EQ(advance(), AYNI_BUSY);
    speech.cancel();
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(calls, 0U);
    EXPECT_TRUE(bodies.values.empty());
}
// The source basis remains fixed even when deletion and file ACK happen between segments.
TEST_F(ReplyVoiceKernel, ClearedAndAcknowledgedContextCannotAuthorizeTheOldRemainingText) {
    grant();
    ASSERT_EQ(start(std::string(2400, 'd')), AYNI_OK);
    ASSERT_TRUE(until([&] {
        const auto result = advance();
        EXPECT_EQ(result, AYNI_BUSY);
        return calls == 1 && speech.goal() == 0;
    }));
    auto context = initialized<ayni_context_info_v1>();
    ASSERT_EQ(ayni_get_context(kernel.get(), session, owner, &context), AYNI_OK);
    auto change = initialized<ayni_context_command_v1>();
    change.context_id = context.context_id;
    change.expected_generation = context.generation;
    change.action = AYNI_CONTEXT_ERASE_ALL;
    ASSERT_EQ(commands.submit(AYNI_INTENT_CONTEXT_FORGET, &change, context.revision).status,
              AYNI_OK);
    ASSERT_TRUE(until([&] { return !commands.pending(); }));
    ASSERT_EQ(ayni_get_context(kernel.get(), session, owner, &context), AYNI_OK);
    change = {};
    change.struct_size = sizeof(change);
    change.context_id = context.context_id;
    change.expected_generation = context.generation;
    ASSERT_EQ(commands.submit(AYNI_INTENT_CONTEXT_ACK, &change, context.revision).status, AYNI_OK);
    ASSERT_TRUE(until([&] { return !commands.pending(); }));
    EXPECT_EQ(advance(), AYNI_EXPIRED);
    EXPECT_EQ(calls, 1U);
    EXPECT_TRUE(bodies.values.empty());
}
// Cancellation acknowledgement is not Task termination: retained PCM/Provider loans drain later.
TEST_F(ReplyVoiceKernel, CancelKeepsTurnAndBodyUntilAsynchronousTaskTerminationIsDurable) {
    grant();
    hold_audio = true;
    delayed_cancel = true;
    ASSERT_EQ(start(std::string(2400, 'e')), AYNI_OK);
    ASSERT_TRUE(until([&] {
        if (calls) {
            return true;
        }
        EXPECT_EQ(advance(), AYNI_BUSY);
        return false;
    }));
    speech.cancel();
    ASSERT_TRUE(until([&] {
        EXPECT_EQ(advance(), AYNI_BUSY);
        return cancellations == 1;
    }));
    auto goal = initialized<ayni_goal_info_v1>();
    ASSERT_EQ(ayni_get_goal(kernel.get(), speech.goal(), &goal), AYNI_OK);
    uint32_t observed = 0;
    ASSERT_TRUE(until([&] {
        EXPECT_EQ(advance(), AYNI_BUSY);
        EXPECT_FALSE(bodies.values.empty());
        return ++observed == 5;
    }));
    finish_cancel();
    EXPECT_EQ(advance(), AYNI_BUSY);
    ayni_status_t result = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        result = advance();
        return result != AYNI_BUSY;
    }));
    EXPECT_EQ(result, AYNI_OK);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_TRUE(bodies.values.empty());
    auto task = initialized<ayni_task_info_v1>();
    auto info = initialized<ayni_kernel_info_v1>();
    ASSERT_EQ(ayni_get_task(kernel.get(), goal.last_task_id, &task), AYNI_OK);
    ASSERT_EQ(ayni_get_kernel_info(kernel.get(), &info), AYNI_OK);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_LE(task.commit_seq, info.durable_seq);
}
} // namespace
