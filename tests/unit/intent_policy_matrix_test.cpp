// Policy matrix (ADR-0051): for every intent kind ayni_submit_intent
// implements, the sessions that may submit it and how a retry of its command
// is recognised. These decide authorization and idempotency, so the expected
// values are written out once here, one row per kind, as the place to review
// them. intent_traits.h must agree with every row, and each column is then
// checked through the C ABI and the command window that read the table.

#include "kernel/service/command/command_table.h"
#include "kernel/service/command/intent_traits.h"
#include "work_node.h"

#include <ayni/authority.h>
#include <ayni/calls.h>
#include <ayni/context.h>
#include <ayni/memory.h>
#include <ayni/scenes.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <vector>

namespace {
using namespace ayni::test;
using ayni::kernel::CommandIdentity;
using ayni::kernel::CommandMapping;
using ayni::kernel::CommandTable;
using ayni::kernel::SessionRule;

struct Row {
    ayni_intent_kind_t kind;
    SessionRule session;
    CommandIdentity identity;
    ayni_object_kind_t object;
};

constexpr SessionRule any = SessionRule::any;
constexpr SessionRule management = SessionRule::management;
constexpr SessionRule management_on_retry = SessionRule::management_on_retry;
constexpr CommandIdentity ordinary = CommandIdentity::ordinary;
constexpr CommandIdentity exact = CommandIdentity::exact;
constexpr CommandIdentity authorization = CommandIdentity::authorization;
constexpr CommandIdentity revoke = CommandIdentity::revoke;

// clang-format off
constexpr Row matrix[] = {
    {AYNI_INTENT_GOAL_CREATE,            any,                 ordinary,      AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_CANCEL,            any,                 ordinary,      AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_PAUSE,             any,                 ordinary,      AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_RESUME,            any,                 ordinary,      AYNI_OBJECT_GOAL},
    {AYNI_INTENT_TASK_CANCEL,            any,                 ordinary,      AYNI_OBJECT_TASK},
    {AYNI_INTENT_INSTANCE_PAUSE,         any,                 ordinary,      AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_INSTANCE_RESUME,        any,                 ordinary,      AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_INSTANCE_STOP,          any,                 ordinary,      AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_GRANT_CREATE,           management,          exact,         AYNI_OBJECT_GRANT},
    {AYNI_INTENT_GRANT_REVOKE,           management,          revoke,        AYNI_OBJECT_GRANT},
    {AYNI_INTENT_GOAL_APPROVE,           management,          authorization, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_REAUTHORIZE,       management_on_retry, authorization, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, management_on_retry, exact,         AYNI_OBJECT_TASK},
    {AYNI_INTENT_CALL_QUERY_RESULT,      management_on_retry, exact,         AYNI_OBJECT_CALL},
    {AYNI_INTENT_TASK_RESUME_WAIT,       management_on_retry, exact,         AYNI_OBJECT_TASK},
    {AYNI_INTENT_SCENE_SET_ENABLED,      management_on_retry, exact,         AYNI_OBJECT_SCENE},
    {AYNI_INTENT_MEMORY_SAVE,            management_on_retry, exact,         AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_MEMORY_CONFIRM,         management_on_retry, exact,         AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_MEMORY_DELETE,          management_on_retry, exact,         AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_CONTEXT_OPEN,           management_on_retry, exact,         AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_PUBLISH,        management_on_retry, exact,         AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_FORGET,         management_on_retry, exact,         AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_ACK,            management_on_retry, exact,         AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_PAUSE_GROUP_SET, management_on_retry, exact, AYNI_OBJECT_PAUSE_GROUP},
    {AYNI_INTENT_UNRESOLVED_OP_QUERY, management_on_retry, exact, AYNI_OBJECT_UNRESOLVED_OP},
    {AYNI_INTENT_UNRESOLVED_OP_ACKNOWLEDGE, management_on_retry, exact, AYNI_OBJECT_UNRESOLVED_OP},
};
// clang-format on

TEST(IntentPolicyMatrix, every_implemented_kind_has_the_reviewed_traits) {
    ASSERT_EQ(std::size(matrix), ayni::kernel::last_intent_kind);
    EXPECT_FALSE(ayni::kernel::implements_intent(0));
    EXPECT_FALSE(ayni::kernel::implements_intent(ayni::kernel::last_intent_kind + 1U));
    for (const Row &row : matrix) {
        SCOPED_TRACE(row.kind);
        ASSERT_TRUE(ayni::kernel::implements_intent(row.kind));
        const auto &traits = ayni::kernel::intent_traits(row.kind);
        EXPECT_EQ(traits.kind, row.kind);
        EXPECT_EQ(traits.session, row.session);
        EXPECT_EQ(traits.identity, row.identity);
        EXPECT_EQ(traits.object, row.object);
    }
}

// A running node with the owner, one agent, a capability, a Scene and a
// MANAGEMENT and a VOICE session of the owner. Each kind has a well-formed
// request whose objects need not exist: every session rule is checked before
// the objects an intent names are looked up.
class IntentPolicy : public ::testing::Test {
  protected:
    void SetUp() override {
        node_.config().grant_capacity = 2;
        node_.config().command_capacity = 2U * ayni::kernel::last_intent_kind;
        node_.config().scene_capacity = 1;
        node_.config().observation_source_capacity = 1;
        node_.on_create([](ayni_kernel_t *kernel) {
            const auto owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
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
            const ayni_scene_rule_v1 rule{text("turn"), 1, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
            const ayni_observation_condition_v1 condition{text("ready"), 1, 0, 1, 1, 500000};
            const ayni_scene_desc_v1 scene{sizeof(scene), 1, text("look"), 1, 1, &rule, &condition};
            ASSERT_EQ(ayni_register_scene(kernel, &scene), AYNI_OK);
        });
        node_.run();
        management_ = open(AYNI_CHANNEL_MANAGEMENT);
        voice_ = open(AYNI_CHANNEL_VOICE);
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        ASSERT_EQ(ayni_get_agent(node_.kernel(), agent_a, &agent), AYNI_OK);
        build_requests(agent.instance_id);
    }

    uint64_t management_session() const { return management_; }
    uint64_t voice_session() const { return voice_; }

    Submitted submit(ayni_intent_kind_t kind, uint64_t command_id, uint64_t session_id) {
        const Request &request = requests_.at(kind - AYNI_INTENT_GOAL_CREATE);
        const ayni_intent_v1 intent{
            sizeof(intent), kind, command_id, session_id, request.expected_revision, 0,
            request.body};
        Submitted out{};
        out.result.struct_size = sizeof(out.result);
        out.error.struct_size = sizeof(out.error);
        out.status = ayni_submit_intent(node_.kernel(), &intent, &out.result, &out.error);
        return out;
    }

  private:
    struct Request {
        const void *body;
        uint32_t expected_revision;
    };

    uint64_t open(ayni_channel_t channel) {
        const auto descriptor = session_desc(channel);
        uint64_t id = 0;
        EXPECT_EQ(ayni_open_session(node_.kernel(), &descriptor, &id), AYNI_OK);
        return id;
    }

    // Ids of this boot that name nothing yet.
    void build_requests(uint64_t instance_id) {
        const uint64_t absent = instance_id + 1000U;
        create_ = WorkNode::create_body(instance_id, 0, "timer");
        goal_ = {sizeof(goal_), 0, absent};
        task_ = {sizeof(task_), 0, absent};
        instance_ = {sizeof(instance_), 0, absent};
        grant_.struct_size = sizeof(grant_);
        grant_.capability_version = 1;
        grant_.capability_id = text("turn");
        grant_.subject_instance_id = instance_id;
        grant_.lifetime = AYNI_GRANT_PERMANENT;
        grant_.scope = {sizeof(ayni_scope_v1), 0, nullptr};
        revoke_ = {sizeof(revoke_), 0, absent};
        approve_.struct_size = sizeof(approve_);
        approve_.missing_mask = 1;
        approve_.goal_id = absent;
        approve_.lifetime = AYNI_GRANT_PERMANENT;
        approve_.original = create_;
        reauthorize_.struct_size = sizeof(reauthorize_);
        reauthorize_.goal_id = absent;
        reauthorize_.expected_authority_commit_seq = 1;
        reauthorize_.original = create_;
        limit_ = {sizeof(limit_), 1, absent};
        query_ = {sizeof(query_), 0, absent};
        unresolved_ = {sizeof(unresolved_), 0, absent};
        scene_ = {sizeof(scene_), 1, text("look"), 1, 0};
        pause_group_ = {sizeof(pause_group_), 1, 1, 0};
        memory_.struct_size = sizeof(memory_);
        memory_.name_space = AYNI_MEMORY_PRIVATE;
        memory_.namespace_id = instance_id;
        memory_.body = text("preference");
        memory_target_ = {sizeof(memory_target_), 0, absent};
        context_open_ = {sizeof(context_open_), AYNI_CONTEXT_IDLE, instance_id, 0, 0, 0, 0};
        context_publish_ = {sizeof(context_publish_), AYNI_CONTEXT_IDLE, 0, absent, 0, absent, 1};
        context_forget_ = {sizeof(context_forget_), AYNI_CONTEXT_ERASE_ALL, 0, absent, 0, 0, 0};
        context_ack_ = {sizeof(context_ack_), AYNI_CONTEXT_IDLE, 0, absent, 0, 0, 0};
        requests_ = {{&create_, 0},         {&goal_, 1},         {&goal_, 1},
                     {&goal_, 1},           {&task_, 1},         {&instance_, 1},
                     {&instance_, 1},       {&instance_, 1},     {&grant_, 0},
                     {&revoke_, 1},         {&approve_, 1},      {&reauthorize_, 1},
                     {&limit_, 1},          {&query_, 1},        {&task_, 1},
                     {&scene_, 0},          {&memory_, 0},       {&memory_target_, 1},
                     {&memory_target_, 1},  {&context_open_, 0}, {&context_publish_, 1},
                     {&context_forget_, 1}, {&context_ack_, 1},  {&pause_group_, 0},
                     {&unresolved_, 1},     {&unresolved_, 1}};
        ASSERT_EQ(requests_.size(), std::size(matrix));
    }

    Node node_;
    uint64_t management_ = 0;
    uint64_t voice_ = 0;
    ayni_goal_create_v1 create_{};
    ayni_goal_target_v1 goal_{};
    ayni_task_target_v1 task_{};
    ayni_instance_target_v1 instance_{};
    ayni_grant_create_v1 grant_{};
    ayni_grant_revoke_v1 revoke_{};
    ayni_goal_approve_v1 approve_{};
    ayni_goal_reauthorize_v1 reauthorize_{};
    ayni_task_raise_model_limit_v1 limit_{};
    ayni_call_query_result_v1 query_{};
    ayni_unresolved_op_target_v1 unresolved_{};
    ayni_scene_set_enabled_v1 scene_{};
    ayni_pause_group_set_v1 pause_group_{};
    ayni_memory_save_v1 memory_{};
    ayni_memory_target_v1 memory_target_{};
    ayni_context_command_v1 context_open_{}, context_publish_{}, context_forget_{}, context_ack_{};
    std::vector<Request> requests_;
};

// A VOICE session of the owner is refused exactly the management kinds; the
// MANAGEMENT session passes every session rule. A refusal for the channel
// also shows the request was well formed, since its form is checked first.
TEST_F(IntentPolicy, the_session_rule_decides_which_sessions_may_submit_each_kind) {
    uint64_t command_id = 0;
    for (const Row &row : matrix) {
        SCOPED_TRACE(row.kind);
        const Submitted voice = submit(row.kind, ++command_id, voice_session());
        if (row.session == any) {
            EXPECT_NE(voice.status, AYNI_DENIED);
            EXPECT_NE(voice.status, AYNI_INVALID_ARGUMENT);
        } else {
            EXPECT_EQ(voice.status, AYNI_DENIED);
            EXPECT_EQ(voice.error.reason, AYNI_REASON_GRANT_CHANNEL);
        }
        const Submitted owner = submit(row.kind, ++command_id, management_session());
        EXPECT_NE(owner.status, AYNI_DENIED);
        EXPECT_NE(owner.status, AYNI_INVALID_ARGUMENT);
    }
}

// An exact command is recognised by its id, so it cannot be submitted
// without one; an ordinary intent may be.
TEST_F(IntentPolicy, exact_kinds_require_a_command_id) {
    for (const Row &row : matrix) {
        SCOPED_TRACE(row.kind);
        const Submitted submitted = submit(row.kind, 0, management_session());
        if (row.identity == ordinary) {
            EXPECT_NE(submitted.status, AYNI_INVALID_ARGUMENT);
        } else {
            EXPECT_EQ(submitted.status, AYNI_INVALID_ARGUMENT);
        }
    }
}

// SessionRule::management is the operation's own first check, so a retry of
// an accepted command returns its first result to any session of the
// principal; management_on_retry is checked before the lookup.
TEST_F(IntentPolicy, a_retry_needs_the_management_session_only_when_the_rule_says_so) {
    const Submitted grant = submit(AYNI_INTENT_GRANT_CREATE, 1, management_session());
    ASSERT_EQ(grant.status, AYNI_OK);
    const Submitted grant_retry = submit(AYNI_INTENT_GRANT_CREATE, 1, voice_session());
    ASSERT_EQ(grant_retry.status, AYNI_OK);
    EXPECT_EQ(grant_retry.result.object.id, grant.result.object.id);
    EXPECT_EQ(grant_retry.result.commit_seq, grant.result.commit_seq);

    const Submitted scene = submit(AYNI_INTENT_SCENE_SET_ENABLED, 2, management_session());
    ASSERT_EQ(scene.status, AYNI_OK);
    const Submitted scene_retry = submit(AYNI_INTENT_SCENE_SET_ENABLED, 2, voice_session());
    EXPECT_EQ(scene_retry.status, AYNI_DENIED);
    EXPECT_EQ(scene_retry.error.reason, AYNI_REASON_GRANT_CHANNEL);
    EXPECT_EQ(submit(AYNI_INTENT_SCENE_SET_ENABLED, 2, management_session()).result.commit_seq,
              scene.result.commit_seq);
}

// The command window keeps a mapping without canonical bytes only for an
// ordinary kind, whether it is accepted now or restored from storage.
TEST(IntentPolicyMatrix, only_ordinary_kinds_map_without_canonical_bytes) {
    constexpr uint64_t principal = 7;
    const CommandTable::Limits limits{1, ayni::kernel::last_intent_kind};
    std::vector<uint64_t> accepted_storage(CommandTable::storage_size(limits) / sizeof(uint64_t) +
                                           1U);
    std::vector<uint64_t> restored_storage(accepted_storage.size());
    CommandTable accepted(accepted_storage.data(), limits);
    CommandTable restored(restored_storage.data(), limits);
    for (const Row &row : matrix) {
        SCOPED_TRACE(row.kind);
        const CommandMapping mapping{principal, row.kind, 1,
                                     row.kind,  row.kind, {0, row.kind, 1, row.object}};
        EXPECT_EQ(accepted.insert(mapping), row.identity == ordinary);
        EXPECT_EQ(restored.restore_checked(mapping, 0), row.identity == ordinary
                                                            ? CommandTable::Restore::restored
                                                            : CommandTable::Restore::corrupt);
    }
}

} // namespace
