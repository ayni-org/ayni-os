// E3 S1 / C-POLICY-006,008: replay exactly the same public common snapshot
// twice through each concrete strategy, with a recording call sink and no I/O.
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/ayni.hpp>
#include <ayni/strategies/builtin.h>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>
namespace {
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
std::string string(ayni_string_v1 v) {
    return {v.data, v.size};
}
struct Requested {
    uint32_t kind = 0;
    std::string key, binding, body;
    uint32_t version = 0;
    static ayni_status_t record(void *ctx, const ayni_call_request_v1 *r) {
        *static_cast<Requested *>(ctx) = {r->kind, string(r->step_key), string(r->binding_id),
                                          string(r->text), r->binding_version};
        return AYNI_OK;
    }
    bool operator==(const Requested &r) const {
        return kind == r.kind && key == r.key && binding == r.binding && body == r.body &&
               version == r.version;
    }
};
TEST(ReplacementPolicy, both_implementations_repeat_the_same_common_snapshot_decision) {
    namespace strategies = ayni::strategies;
    using ayni::initialized;
    strategies::ConversationRecord conversation;
    conversation.id = 1;
    conversation.owner_instance_id = 1;
    strategies::Encoded parameters;
    ASSERT_EQ(strategies::Conversation::begin(conversation, text("fixture input"), parameters),
              AYNI_OK);
    strategies::ToolLoop loop;
    strategies::ToolLoopConfig config;
    config.identity.id = text("replacement.loop");
    config.model = {AYNI_POLICY_REQUEST_MODEL,
                    text("model"),
                    1,
                    {sizeof(ayni_scope_v1), 0, nullptr},
                    text("Reply briefly.")};
    config.summary = config.model;
    ASSERT_EQ(loop.configure(config), AYNI_OK);
    const ayni_task_step_v1 steps[] = {{text("increment")}, {text("checkpoint.ready")}};
    auto snapshot = initialized<ayni_task_snapshot_v1>();
    snapshot.goal_type_id = text("replacement.workflow");
    snapshot.goal_type_version = 1;
    snapshot.owner_instance_id = conversation.owner_instance_id;
    snapshot.goal_parameters = parameters.view();
    snapshot.checkpoint.strategy_id = text("replacement.rules");
    snapshot.checkpoint.strategy_version = 1;
    snapshot.steps = steps;
    snapshot.step_count = static_cast<uint32_t>(std::size(steps));
    snapshot.step_capacity = strategies::rule_steps_max;
    snapshot.call_result = [](const void *, ayni_string_v1 key, ayni_task_call_result_v1 *r) {
        if (string(key) != "increment") {
            return AYNI_NOT_FOUND;
        }
        *r = initialized<ayni_task_call_result_v1>();
        r->outcome = AYNI_CALL_COMPLETED;
        r->effect = AYNI_CALL_EFFECT_COMPLETE;
        return AYNI_OK;
    };
    for (const auto &strategy :
         {ayni::applications::experiment_fixtures::replacement::rule_strategy(), loop.describe()}) {
        Requested requests[2];
        std::vector<uint8_t> checkpoints[2];
        for (uint32_t at = 0; at < std::size(requests); ++at) {
            std::array<uint8_t, strategies::state_bytes_max> checkpoint{};
            ayni_policy_intent_v1 intent{};
            auto output = initialized<ayni_policy_output_v1>();
            output.intents = &intent;
            output.intent_capacity = 1;
            output.checkpoint = checkpoint.data();
            output.checkpoint_capacity = strategy.max_checkpoint_bytes;
            output.call_context = &requests[at];
            output.request_call = Requested::record;
            ASSERT_EQ(strategy.decide(strategy.ctx, &snapshot, &output), AYNI_OK);
            EXPECT_EQ(requests[at].key, "round.0.model.v1");
            EXPECT_EQ(requests[at].binding, "model");
            EXPECT_EQ(requests[at].kind, AYNI_POLICY_REQUEST_MODEL);
            checkpoints[at].assign(checkpoint.begin(), checkpoint.begin() + output.checkpoint_size);
        }
        EXPECT_TRUE(requests[0] == requests[1]);
        EXPECT_EQ(checkpoints[0], checkpoints[1]);
    }
}
} // namespace
