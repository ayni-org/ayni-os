#include "protocol.h"
#include "reply_voice.h"
#include <ayni/calls.h>
#include <ayni/values.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
namespace {
struct SpeechDecision {
    bool completed = false;
    uint32_t calls = 0;
    std::string spoken;
    std::string stored;
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                              uint32_t capacity) {
        const auto &value = static_cast<SpeechDecision *>(context)->stored;
        if (ref->id != 1 || capacity < value.size()) {
            return AYNI_NOT_FOUND;
        }
        std::memcpy(out, value.data(), value.size());
        return AYNI_OK;
    }
    ayni_policy_output_v1 *output = nullptr;
    static ayni_status_t result(const void *context, ayni_string_v1,
                                ayni_task_call_result_v1 *out) {
        if (!static_cast<const SpeechDecision *>(context)->completed) {
            return AYNI_NOT_FOUND;
        }
        *out = {};
        out->struct_size = sizeof(*out);
        out->outcome = AYNI_CALL_COMPLETED;
        out->status = AYNI_OK;
        out->effect = AYNI_CALL_EFFECT_COMPLETE;
        return AYNI_OK;
    }
    static ayni_status_t request(void *context, const ayni_call_request_v1 *call) {
        auto &self = *static_cast<SpeechDecision *>(context);
        ayni::ValueReader reader;
        EXPECT_EQ(reader.read(call->value), AYNI_OK);
        const auto *value = reader.field(1);
        if (!value || value->kind != AYNI_VALUE_TEXT) {
            return AYNI_INVALID_ARGUMENT;
        }
        self.spoken.assign(value->value.text.data, value->value.text.size);
        ++self.calls;
        self.output->intent_count = 1;
        return AYNI_OK;
    }
};
} // namespace
// User 2026-11-13: a deterministic Rules follow-up uses the existing explicit speech binding.
TEST(WifiVoiceReply, RequestsExistingSpeechCapabilityOnceAndFinishesFromItsLedger) {
    const std::string answer = "这段最终答复会自动朗读。";
    SpeechDecision serving;
    serving.stored = answer;
    const ayni_body_store_v1 bodies{
        sizeof(bodies), 0,       &serving, nullptr, SpeechDecision::read,
        nullptr,        nullptr, nullptr,  nullptr, nullptr};
    wifichat::ReplyVoice voice;
    const auto policy = voice.strategy(&bodies);
    auto snapshot = ayni::initialized<ayni_task_snapshot_v1>();
    snapshot.goal_type_version = wifichat::ReplyVoice::version;
    snapshot.goal_type_id = {wifichat::ReplyVoice::type_id,
                             uint32_t(std::strlen(wifichat::ReplyVoice::type_id))};
    std::array<uint8_t, wifichat::body_reference_bytes> parameters{};
    const ayni_body_ref_v1 reference{
        1,
        ayni_body_digest(
            {reinterpret_cast<const uint8_t *>(answer.data()), uint32_t(answer.size())}),
        uint32_t(answer.size()), 0};
    wifichat::put_body_reference(parameters.data(), reference);
    snapshot.goal_parameters = {parameters.data(), uint32_t(parameters.size())};
    snapshot.step_capacity = 1;
    snapshot.call_context = &serving;
    snapshot.call_result = SpeechDecision::result;
    auto output = ayni::initialized<ayni_policy_output_v1>();
    ayni_policy_intent_v1 intent{};
    output.intents = &intent;
    output.intent_capacity = 1;
    output.call_context = &serving;
    output.request_call = SpeechDecision::request;
    serving.output = &output;
    ASSERT_EQ(policy.decide(policy.ctx, &snapshot, &output), AYNI_OK);
    EXPECT_EQ(serving.calls, 1U);
    EXPECT_EQ(serving.spoken, answer);
    serving.completed = true;
    output.intent_count = 0;
    ASSERT_EQ(policy.decide(policy.ctx, &snapshot, &output), AYNI_OK);
    EXPECT_EQ(serving.calls, 1U);
    ASSERT_EQ(output.intent_count, 1U);
    EXPECT_EQ(intent.kind, AYNI_POLICY_REPORT_COMPLETION);
}
