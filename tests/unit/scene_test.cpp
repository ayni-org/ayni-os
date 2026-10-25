// ADR-0050: public owner preferences, durable enable gate, exact commands and finite declarations.
#include "authority/scene/scene_records.h"
#include "work_node.h"
#include <ayni/scenes.h>

namespace {
using namespace ayni::test;
class SceneApi : public ::testing::Test {
  protected:
    void SetUp() override {
        node.config().scene_capacity = 1;
        node.config().observation_source_capacity = 1;
        node.config().storage_buffer_bytes = 0;
        node.on_create([this](ayni_kernel_t *kernel) {
            const auto owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const ayni_capability_desc_v1 capability{
                sizeof(capability), 1, text("turn"), 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &capability), AYNI_OK);
            const ayni_observation_source_v1 source{sizeof(source),
                                                    1,
                                                    text("ready"),
                                                    AYNI_OBSERVATION_BOOL,
                                                    AYNI_OBSERVATION_MEASURED,
                                                    text("bool"),
                                                    1000000};
            ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
            if (register_scene) {
                EXPECT_EQ(add_scene(kernel), AYNI_OK);
            }
        });
    }
    ayni_status_t add_scene(ayni_kernel_t *kernel, uint32_t source_version = 1) {
        const ayni_scene_rule_v1 rule{text("turn"), 1, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
        const ayni_observation_condition_v1 condition{text("ready"), source_version, 0, 1, 1,
                                                      500000};
        const ayni_scene_desc_v1 scene{sizeof(scene), 1, text("look"), 1, 1, &rule, &condition};
        return ayni_register_scene(kernel, &scene);
    }
    void run() {
        node.run();
        const auto descriptor = session_desc(AYNI_CHANNEL_MANAGEMENT);
        ASSERT_EQ(ayni_open_session(node.kernel(), &descriptor, &session), AYNI_OK);
    }
    Submitted change(uint64_t command, uint32_t revision, bool enable, uint64_t through = 0) {
        const ayni_scene_set_enabled_v1 body{sizeof(body), enable ? 1U : 0U, text("look"),
                                             enable ? 1U : 0U, 0};
        const ayni_intent_v1 intent{sizeof(intent), AYNI_INTENT_SCENE_SET_ENABLED,
                                    command,        through == 0 ? session : through,
                                    revision,       0,
                                    &body};
        Submitted result{};
        result.result.struct_size = sizeof(result.result);
        result.error.struct_size = sizeof(result.error);
        result.status = ayni_submit_intent(node.kernel(), &intent, &result.result, &result.error);
        return result;
    }
    ayni_scene_info_v1 info() {
        ayni_scene_info_v1 result{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(ayni_get_scene(node.kernel(), text("look"), &result), AYNI_OK);
        return result;
    }
    void store(uint64_t sequence) {
        for (unsigned round = 0; round < 100 && node.info().durable_seq < sequence; ++round) {
            node.sim().advance_us(10000);
            node.step();
            node.sim().complete_storage();
        }
        ASSERT_GE(node.info().durable_seq, sequence);
    }
    Node node;
    uint64_t session = 0;
    bool register_scene = true;
};
TEST_F(SceneApi, disabled_default_enable_waits_for_durability_close_blocks_immediately) {
    run();
    EXPECT_EQ(info().revision, 0U);
    EXPECT_EQ(info().consumable, 0U);
    const auto enabled = change(1, 0, true);
    ASSERT_EQ(enabled.status, AYNI_OK);
    EXPECT_EQ(info().enabled, 1U);
    EXPECT_EQ(info().consumable, 0U);
    store(enabled.result.commit_seq);
    EXPECT_EQ(info().consumable, 1U);
    const auto disabled = change(2, 1, false);
    ASSERT_EQ(disabled.status, AYNI_OK);
    EXPECT_GT(disabled.result.commit_seq, node.info().durable_seq);
    EXPECT_EQ(info().consumable, 0U);
    EXPECT_EQ(info().revision, 2U);
}
TEST_F(SceneApi, retries_keep_original_receipt_and_no_change_keeps_scene_revision) {
    run();
    const auto enabled = change(1, 0, true);
    ASSERT_EQ(enabled.status, AYNI_OK);
    const auto retry = change(1, 0, true);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, enabled.result.commit_seq);
    EXPECT_EQ(change(1, 0, false).status, AYNI_IDEMPOTENCY_CONFLICT);
    EXPECT_EQ(change(2, 0, false).status, AYNI_REVISION_CONFLICT);
    const auto unchanged = change(2, 1, true);
    ASSERT_EQ(unchanged.status, AYNI_OK);
    EXPECT_EQ(unchanged.result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    EXPECT_EQ(info().revision, 1U);
    store(unchanged.result.commit_seq);
    ayni_intent_receipt_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    ASSERT_EQ(ayni_get_intent_receipt(node.kernel(), owner_id, 2, &receipt), AYNI_OK);
    EXPECT_EQ(receipt.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
}
TEST_F(SceneApi, conversation_session_cannot_enable_and_zero_command_is_rejected) {
    run();
    uint64_t conversation = 0;
    const auto descriptor = session_desc(AYNI_CHANNEL_VOICE);
    ASSERT_EQ(ayni_open_session(node.kernel(), &descriptor, &conversation), AYNI_OK);
    EXPECT_EQ(change(1, 0, true, conversation).status, AYNI_DENIED);
    EXPECT_EQ(change(0, 0, true).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info().revision, 0U);
}
TEST_F(SceneApi, invalid_cross_reference_does_not_consume_capacity) {
    register_scene = false;
    node.create();
    EXPECT_EQ(add_scene(node.kernel(), 2), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(add_scene(node.kernel()), AYNI_OK);
    const ayni_scene_rule_v1 rule{text("turn"), 1, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
    const ayni_scene_desc_v1 second{sizeof(second), 1, text("other"), 1, 0, &rule, nullptr};
    EXPECT_EQ(ayni_register_scene(node.kernel(), &second), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(info().registered_version, 1U);
}
TEST_F(SceneApi, original_definition_record_round_trip_preserves_constraints_and_preference) {
    run();
    ASSERT_EQ(change(1, 0, true).status, AYNI_OK);
    // White-box: one codec round trip checks stored representation, not a recovery campaign.
    const auto &scenes = node.kernel()->work.scenes;
    const auto &state = scenes.state(0);
    std::vector<uint8_t> bytes(ayni::authority::scene_record_size(state));
    ayni::authority::write_scene_record(state, scenes.state_definition(0), bytes.data());
    ayni::authority::SceneRecord decoded{};
    ASSERT_TRUE(ayni::authority::read_scene_record(bytes.data(),
                                                   static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.state.object_id, state.object_id);
    EXPECT_EQ(decoded.state.mode, ayni::authority::SceneMode::enabled);
    EXPECT_EQ(decoded.definition.size, scenes.state_definition(0).size);
    EXPECT_EQ(std::memcmp(decoded.definition.data, scenes.state_definition(0).data,
                          decoded.definition.size),
              0);
}
} // namespace
