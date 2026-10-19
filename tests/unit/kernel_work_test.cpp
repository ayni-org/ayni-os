// The work model through the C ABI (ADR-0017): registration, sessions, goal
// intents, their check order and error detail, queries, ids across reboots
// and the return values in each kernel state. Runs on the simulation Host.

#include "kernel/kernel_state.h"
#include "kernel_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace {

using ayni::test::Node;

constexpr uint64_t owner_id = 7;
constexpr uint32_t owner_binding = 3;
constexpr uint64_t agent_a = 100;
constexpr uint64_t agent_b = 200;

ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}

// The reminder goal type's validator: at most 4 bytes, and not starting with
// 0xFF. Counts its calls so tests can see when it runs.
struct ReminderRules {
    static ayni_status_t validate(void *ctx, ayni_bytes_v1 parameters) {
        auto &self = *static_cast<ReminderRules *>(ctx);
        ++self.calls;
        self.last.assign(parameters.data, parameters.data + parameters.size);
        if (self.during) {
            // Runs once, from a copy, so that it may call the kernel freely.
            const std::function<void()> during = std::move(self.during);
            self.during = nullptr;
            during();
        }
        if (parameters.size > 4U || (parameters.size > 0U && parameters.data[0] == 0xFFU)) {
            return AYNI_INVALID_ARGUMENT;
        }
        return AYNI_OK;
    }
    int calls = 0;
    std::vector<uint8_t> last;
    std::function<void()> during; // runs once inside the next validation
};

ayni_principal_desc_v1 owner_desc(uint64_t principal_id = owner_id) {
    ayni_principal_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.kind = AYNI_PRINCIPAL_OWNER;
    desc.principal_id = principal_id;
    desc.binding_revision = owner_binding;
    return desc;
}

ayni_agent_desc_v1 agent_desc(uint64_t agent_id) {
    ayni_agent_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.agent_id = agent_id;
    return desc;
}

ayni_goal_type_v1 goal_type_desc(const char *id, uint32_t version,
                                 ayni_goal_validate_fn validate = nullptr, void *ctx = nullptr) {
    ayni_goal_type_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.version = version;
    desc.goal_type_id = text(id);
    desc.ctx = ctx;
    desc.validate = validate;
    return desc;
}

ayni_session_desc_v1 session_desc(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT,
                                  uint64_t principal_id = owner_id) {
    ayni_session_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.channel = channel;
    desc.principal_id = principal_id;
    return desc;
}

struct Submitted {
    ayni_status_t status;
    ayni_intent_result_v1 result;
    ayni_error_v1 error;
};

// A node with an owner, two agents and three goal type versions registered
// on every kernel it creates.
class WorkNode {
  public:
    WorkNode() {
        node_.on_create([this](ayni_kernel_t *kernel) {
            const ayni_principal_desc_v1 owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            for (const uint64_t agent_id : {agent_a, agent_b}) {
                const ayni_agent_desc_v1 agent = agent_desc(agent_id);
                ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            }
            const ayni_goal_type_v1 types[] = {
                goal_type_desc("reminder", 1, &ReminderRules::validate, &rules_),
                goal_type_desc("timer", 1),
            };
            for (const ayni_goal_type_v1 &type : types) {
                ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            }
        });
    }

    Node &node() { return node_; }
    ayni_kernel_t *kernel() { return node_.kernel(); }
    ReminderRules &rules() { return rules_; }

    // Into RUNNING with one management session open.
    void run() {
        node_.run();
        session_ = open_session();
    }

    uint64_t session() const { return session_; }

    uint64_t open_session(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT) {
        const ayni_session_desc_v1 desc = session_desc(channel);
        uint64_t session_id = 0;
        EXPECT_EQ(ayni_open_session(kernel(), &desc, &session_id), AYNI_OK);
        return session_id;
    }

    ayni_agent_info_v1 agent(uint64_t agent_id) {
        ayni_agent_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_agent(kernel(), agent_id, &info), AYNI_OK);
        return info;
    }

    uint64_t instance(uint64_t agent_id) { return agent(agent_id).instance_id; }

    ayni_goal_info_v1 goal(uint64_t goal_id) {
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal(kernel(), goal_id, &info), AYNI_OK);
        return info;
    }

    Submitted submit(const ayni_intent_v1 &intent) {
        Submitted out{};
        out.result.struct_size = sizeof(out.result);
        out.error.struct_size = sizeof(out.error);
        out.status = ayni_submit_intent(kernel(), &intent, &out.result, &out.error);
        EXPECT_EQ(out.error.status, out.status);
        return out;
    }

    static ayni_goal_create_v1 create_body(uint64_t owner_instance_id, uint64_t parent_goal_id = 0,
                                           const char *type = "reminder", uint32_t version = 1,
                                           ayni_bytes_v1 parameters = {nullptr, 0}) {
        ayni_goal_create_v1 body{};
        body.struct_size = sizeof(body);
        body.goal_type_version = version;
        body.goal_type_id = text(type);
        body.owner_instance_id = owner_instance_id;
        body.parent_goal_id = parent_goal_id;
        body.parameters = parameters;
        return body;
    }

    ayni_intent_v1 intent(ayni_intent_kind_t kind, const void *body,
                          uint32_t expected_revision = 0) const {
        ayni_intent_v1 intent{};
        intent.struct_size = sizeof(intent);
        intent.kind = kind;
        intent.session_id = session_;
        intent.expected_revision = expected_revision;
        intent.body = body;
        return intent;
    }

    Submitted create(const ayni_goal_create_v1 &body) {
        return submit(intent(AYNI_INTENT_GOAL_CREATE, &body));
    }

    // Creates a goal owned by agent_a's instance and returns its id.
    uint64_t create_goal(uint64_t parent_goal_id = 0) {
        const Submitted created = create(create_body(instance(agent_a), parent_goal_id));
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }

    Submitted change(ayni_intent_kind_t kind, uint64_t goal_id, uint32_t expected_revision) {
        ayni_goal_target_v1 body{};
        body.struct_size = sizeof(body);
        body.goal_id = goal_id;
        return submit(intent(kind, &body, expected_revision));
    }

    // Changes the goal at its current revision.
    Submitted change(ayni_intent_kind_t kind, uint64_t goal_id) {
        return change(kind, goal_id, goal(goal_id).revision);
    }

  private:
    ReminderRules rules_;
    Node node_;
    uint64_t session_ = 0;
};

void expect_refused(const Submitted &submitted, ayni_status_t status, ayni_reason_t reason,
                    ayni_object_kind_t kind = 0, uint64_t id = 0) {
    EXPECT_EQ(submitted.status, status);
    EXPECT_EQ(submitted.error.reason, reason);
    EXPECT_EQ(submitted.error.object.kind, kind);
    EXPECT_EQ(submitted.error.object.id, id);
    EXPECT_EQ(submitted.error.reserved, 0U);
}

// --- Registration ---------------------------------------------------------

TEST(WorkModel, registration_checks_its_arguments) {
    Node node;
    node.create();
    ayni_kernel_t *kernel = node.kernel();

    EXPECT_EQ(ayni_register_principal(nullptr, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_principal(kernel, nullptr), AYNI_INVALID_ARGUMENT);
    ayni_principal_desc_v1 principal = owner_desc();
    principal.struct_size -= 1U;
    EXPECT_EQ(ayni_register_principal(kernel, &principal), AYNI_INVALID_ARGUMENT);
    principal.struct_size = sizeof(principal) + 8U;
    EXPECT_EQ(ayni_register_principal(kernel, &principal), AYNI_UNSUPPORTED_VERSION);
    for (int field = 0; field < 4; ++field) {
        principal = owner_desc();
        switch (field) {
        case 0:
            principal.kind = 2;
            break;
        case 1:
            principal.principal_id = 0;
            break;
        case 2:
            principal.binding_revision = 0;
            break;
        default:
            principal.reserved = 1;
            break;
        }
        EXPECT_EQ(ayni_register_principal(kernel, &principal), AYNI_INVALID_ARGUMENT) << field;
    }

    EXPECT_EQ(ayni_register_agent(kernel, nullptr), AYNI_INVALID_ARGUMENT);
    ayni_agent_desc_v1 agent = agent_desc(0);
    EXPECT_EQ(ayni_register_agent(kernel, &agent), AYNI_INVALID_ARGUMENT);
    agent = agent_desc(agent_a);
    agent.reserved = 1;
    EXPECT_EQ(ayni_register_agent(kernel, &agent), AYNI_INVALID_ARGUMENT);
    agent = agent_desc(agent_a);
    agent.struct_size = sizeof(agent) + 8U;
    EXPECT_EQ(ayni_register_agent(kernel, &agent), AYNI_UNSUPPORTED_VERSION);

    EXPECT_EQ(ayni_register_goal_type(kernel, nullptr), AYNI_INVALID_ARGUMENT);
    for (const ayni_goal_type_v1 &type :
         {goal_type_desc("reminder", 0), goal_type_desc("", 1), goal_type_desc("Reminder", 1),
          goal_type_desc("abcdefghijklmnopqrstuvwxyz0123456", 1)}) {
        EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_INVALID_ARGUMENT);
    }
    ayni_goal_type_v1 type = goal_type_desc("reminder", 1);
    type.struct_size -= 1U;
    EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_INVALID_ARGUMENT);
}

// C-PROD-001 and the configured capacities: a refused registration changes
// nothing.
TEST(WorkModel, registration_refuses_duplicates_and_capacity) {
    Node node;
    node.config().agent_capacity = 1;
    node.config().goal_type_capacity = 1;
    node.create();
    ayni_kernel_t *kernel = node.kernel();

    const ayni_principal_desc_v1 owner = owner_desc();
    ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
    const ayni_principal_desc_v1 second_owner = owner_desc(owner_id + 1U);
    EXPECT_EQ(ayni_register_principal(kernel, &second_owner), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(ayni_register_principal(kernel, &owner), AYNI_CAPACITY_EXCEEDED);

    const ayni_agent_desc_v1 first = agent_desc(agent_a);
    const ayni_agent_desc_v1 second = agent_desc(agent_b);
    ASSERT_EQ(ayni_register_agent(kernel, &first), AYNI_OK);
    EXPECT_EQ(ayni_register_agent(kernel, &first), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_agent(kernel, &second), AYNI_CAPACITY_EXCEEDED);

    const ayni_goal_type_v1 reminder = goal_type_desc("reminder", 1);
    const ayni_goal_type_v1 reminder_v2 = goal_type_desc("reminder", 2);
    ASSERT_EQ(ayni_register_goal_type(kernel, &reminder), AYNI_OK);
    EXPECT_EQ(ayni_register_goal_type(kernel, &reminder), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_goal_type(kernel, &reminder_v2), AYNI_CAPACITY_EXCEEDED);
}

// --- Agents ---------------------------------------------------------------

// Instances start in one transition when the kernel enters RUNNING; the
// transition follows the boot batch and is not durable yet.
TEST(WorkModel, instances_start_with_the_kernel_in_one_transition) {
    WorkNode work;
    work.node().create_and_start();
    work.node().step();
    ayni_agent_info_v1 before{};
    before.struct_size = sizeof(before);
    ASSERT_EQ(ayni_get_agent(work.kernel(), agent_a, &before), AYNI_OK);
    EXPECT_EQ(before.lifecycle, AYNI_AGENT_REGISTERED);
    EXPECT_EQ(before.instance_id, 0U);
    EXPECT_EQ(before.commit_seq, 0U);

    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const ayni_agent_info_v1 a = work.agent(agent_a);
    const ayni_agent_info_v1 b = work.agent(agent_b);
    EXPECT_EQ(a.lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(a.agent_id, agent_a);
    EXPECT_EQ(a.instance_id >> 32U, 1U); // the boot epoch
    EXPECT_NE(a.instance_id, b.instance_id);
    EXPECT_EQ(a.commit_seq, 2U); // the boot batch is commit_seq 1
    EXPECT_EQ(b.commit_seq, a.commit_seq);
    EXPECT_EQ(work.node().info().durable_seq, 1U);

    ayni_agent_info_v1 unknown{};
    unknown.struct_size = sizeof(unknown);
    EXPECT_EQ(ayni_get_agent(work.kernel(), 999, &unknown), AYNI_NOT_FOUND);
}

TEST(WorkModel, queries_check_their_arguments) {
    WorkNode work;
    work.run();
    ayni_agent_info_v1 agent{};
    ayni_goal_info_v1 goal{};
    EXPECT_EQ(ayni_get_agent(nullptr, agent_a, &agent), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_agent(work.kernel(), agent_a, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_goal(work.kernel(), 1, nullptr), AYNI_INVALID_ARGUMENT);
    agent.struct_size = sizeof(agent) - 1U;
    EXPECT_EQ(ayni_get_agent(work.kernel(), agent_a, &agent), AYNI_INVALID_ARGUMENT);
    agent.struct_size = sizeof(agent) + 8U;
    EXPECT_EQ(ayni_get_agent(work.kernel(), agent_a, &agent), AYNI_UNSUPPORTED_VERSION);
    goal.struct_size = sizeof(goal) - 1U;
    EXPECT_EQ(ayni_get_goal(work.kernel(), 1, &goal), AYNI_INVALID_ARGUMENT);
    goal.struct_size = sizeof(goal) + 8U;
    EXPECT_EQ(ayni_get_goal(work.kernel(), 1, &goal), AYNI_UNSUPPORTED_VERSION);
}

// --- Sessions -------------------------------------------------------------

// C-AUTH-010: the host opens sessions for a principal it has registered.
TEST(WorkModel, sessions_open_for_a_registered_principal_up_to_capacity) {
    WorkNode work;
    work.node().run();
    uint64_t session_id = 5;
    EXPECT_EQ(ayni_open_session(work.kernel(), nullptr, &session_id), AYNI_INVALID_ARGUMENT);
    ayni_session_desc_v1 desc = session_desc();
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, nullptr), AYNI_INVALID_ARGUMENT);
    desc.struct_size += 8U;
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_UNSUPPORTED_VERSION);
    for (const ayni_channel_t channel : {0U, 3U}) {
        desc = session_desc(channel);
        EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_INVALID_ARGUMENT);
    }
    desc = session_desc(AYNI_CHANNEL_VOICE, owner_id + 1U);
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_NOT_FOUND);
    EXPECT_EQ(session_id, 5U);

    const uint64_t voice = work.open_session(AYNI_CHANNEL_VOICE);
    const uint64_t management = work.open_session(AYNI_CHANNEL_MANAGEMENT);
    EXPECT_NE(voice, 0U);
    EXPECT_NE(voice, management);
    desc = session_desc();
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(session_id, 5U);

    EXPECT_EQ(ayni_close_session(nullptr, voice), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_close_session(work.kernel(), voice), AYNI_OK);
    EXPECT_EQ(ayni_close_session(work.kernel(), voice), AYNI_NOT_FOUND);
    EXPECT_NE(work.open_session(), voice);
}

// A closed session no longer authorizes anything.
TEST(WorkModel, intent_through_a_closed_or_unknown_session_is_denied) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.session_id = 12345;
    expect_refused(work.submit(intent), AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                   AYNI_OBJECT_SESSION, 12345);

    ASSERT_EQ(ayni_close_session(work.kernel(), work.session()), AYNI_OK);
    expect_refused(work.create(body), AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN, AYNI_OBJECT_SESSION,
                   work.session());
}

// --- Creating goals -------------------------------------------------------

// C-COMMIT-003, C-COMMIT-004: the goal is applied at once and queryable, but
// its commit_seq is beyond durable_seq.
TEST(WorkModel, created_goal_is_applied_at_once_and_not_yet_durable) {
    WorkNode work;
    work.run();
    const uint64_t voice = work.open_session(AYNI_CHANNEL_VOICE);
    const uint8_t parameters[] = {1, 2, 3};
    ayni_goal_create_v1 body =
        WorkNode::create_body(work.instance(agent_b), 0, "reminder", 1, {parameters, 3});
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.session_id = voice;
    const Submitted created = work.submit(intent);
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(created.error.reason, AYNI_REASON_NONE);
    EXPECT_EQ(created.result.struct_size, sizeof(created.result));
    EXPECT_EQ(created.result.object.kind, AYNI_OBJECT_GOAL);
    EXPECT_EQ(created.result.object.origin_device_id, 0U);
    EXPECT_EQ(created.result.object.revision, 1U);
    EXPECT_EQ(created.result.object.id >> 32U, 1U);
    EXPECT_EQ(created.result.commit_seq, 3U); // boot batch 1, agents 2
    EXPECT_GT(created.result.commit_seq, work.node().info().durable_seq);
    EXPECT_EQ(work.rules().last, (std::vector<uint8_t>{1, 2, 3}));

    const ayni_goal_info_v1 goal = work.goal(created.result.object.id);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goal.goal_id, created.result.object.id);
    EXPECT_EQ(goal.parent_goal_id, 0U);
    EXPECT_EQ(goal.owner_instance_id, work.instance(agent_b));
    EXPECT_EQ(goal.origin, AYNI_ORIGIN_USER);
    EXPECT_EQ(goal.origin_channel, AYNI_CHANNEL_VOICE);
    EXPECT_EQ(goal.origin_principal_id, owner_id);
    EXPECT_EQ(goal.origin_binding_revision, owner_binding);
    EXPECT_EQ(goal.commit_seq, created.result.commit_seq);
    EXPECT_EQ(goal.transition_id, goal.commit_seq + 0U); // both start after the boot batch
    EXPECT_EQ(goal.revision, 1U);
    EXPECT_EQ(goal.cancel_revision, 0U);
    EXPECT_EQ(goal.depth, 0U);
    EXPECT_EQ(goal.goal_type_version, 1U);
    ASSERT_EQ(goal.goal_type_id_size, 8U);
    EXPECT_EQ(std::memcmp(goal.goal_type_id, "reminder", 8), 0);

    ayni_goal_info_v1 missing{};
    missing.struct_size = sizeof(missing);
    EXPECT_EQ(ayni_get_goal(work.kernel(), created.result.object.id + 1U, &missing),
              AYNI_NOT_FOUND);
}

// Outputs are optional; the intent still applies.
TEST(WorkModel, intent_without_outputs_still_applies) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    const ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    ASSERT_EQ(ayni_submit_intent(work.kernel(), &intent, nullptr, nullptr), AYNI_OK);
    // The instances took sequence numbers 1 and 2, the session 3, that goal 4.
    EXPECT_EQ(work.create_goal(), (uint64_t{1} << 32U) | 5U);
}

// C-POLICY-009, E4.S5.unknown_goal_type_rejected.
TEST(WorkModel, unknown_goal_type_or_version_is_refused) {
    WorkNode work;
    work.run();
    const uint64_t owner = work.instance(agent_a);
    expect_refused(work.create(WorkNode::create_body(owner, 0, "alarm")), AYNI_NOT_FOUND,
                   AYNI_REASON_GOAL_TYPE_UNKNOWN, AYNI_OBJECT_GOAL_TYPE);
    expect_refused(work.create(WorkNode::create_body(owner, 0, "reminder", 2)),
                   AYNI_UNSUPPORTED_VERSION, AYNI_REASON_GOAL_TYPE_VERSION, AYNI_OBJECT_GOAL_TYPE);
}

TEST(WorkModel, owner_must_be_an_instance_of_this_kernel) {
    WorkNode work;
    work.run();
    for (const uint64_t owner : {uint64_t{0}, uint64_t{agent_a}, work.instance(agent_a) + 9U}) {
        expect_refused(work.create(WorkNode::create_body(owner)), AYNI_NOT_FOUND,
                       AYNI_REASON_OWNER_UNKNOWN, AYNI_OBJECT_AGENT_INSTANCE, owner);
    }
}

// The goal type's validator decides the parameters, and it runs only for a
// goal that nothing else refuses.
TEST(WorkModel, parameters_are_checked_last_by_size_and_by_the_goal_type) {
    WorkNode work;
    work.node().config().goal_parameter_max_bytes = 3;
    work.run();
    const uint64_t owner = work.instance(agent_a);
    const uint8_t refused[] = {0xFF};
    const uint8_t too_long[] = {1, 2, 3, 4};
    expect_refused(work.create(WorkNode::create_body(owner, 0, "reminder", 1, {refused, 1})),
                   AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS, AYNI_OBJECT_GOAL_TYPE);
    EXPECT_EQ(work.rules().calls, 1);
    expect_refused(work.create(WorkNode::create_body(owner, 0, "reminder", 1, {too_long, 4})),
                   AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS, AYNI_OBJECT_GOAL_TYPE);
    EXPECT_EQ(work.rules().calls, 1); // the size refused it first
    // Without a validator, a goal type takes no parameters.
    expect_refused(work.create(WorkNode::create_body(owner, 0, "timer", 1, {refused, 1})),
                   AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS, AYNI_OBJECT_GOAL_TYPE);
    EXPECT_EQ(work.create(WorkNode::create_body(owner, 0, "timer", 1)).status, AYNI_OK);
    // Any other refusal comes before the validator runs.
    expect_refused(work.create(WorkNode::create_body(owner + 9U, 0, "reminder", 1, {refused, 1})),
                   AYNI_NOT_FOUND, AYNI_REASON_OWNER_UNKNOWN, AYNI_OBJECT_AGENT_INSTANCE,
                   owner + 9U);
    EXPECT_EQ(work.rules().calls, 1);
}

// C-LIFE-020, C-LIMIT-007: a child keeps its parent's owner, and the tree is
// bounded in depth and width.
TEST(WorkModel, child_goals_follow_their_parent_and_the_tree_limits) {
    WorkNode work;
    work.node().config().goal_depth_max = 1;
    work.node().config().goal_children_max = 1;
    work.run();
    const uint64_t root = work.create_goal();
    const uint64_t child = work.create_goal(root);
    ASSERT_NE(child, 0U);
    EXPECT_EQ(work.goal(child).depth, 1U);
    EXPECT_EQ(work.goal(child).parent_goal_id, root);

    expect_refused(work.create(WorkNode::create_body(work.instance(agent_b), root)),
                   AYNI_INVALID_ARGUMENT, AYNI_REASON_PARENT_OWNER, AYNI_OBJECT_GOAL, root);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), child)),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_DEPTH, AYNI_OBJECT_GOAL, child);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), root)),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CHILDREN, AYNI_OBJECT_GOAL, root);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), 4242)), AYNI_NOT_FOUND,
                   AYNI_REASON_PARENT_UNKNOWN, AYNI_OBJECT_GOAL, 4242);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, root).status, AYNI_OK);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), root)),
                   AYNI_INVALID_STATE, AYNI_REASON_PARENT_STATE, AYNI_OBJECT_GOAL, root);
}

// A config that ends before goal_depth_max gets the default tree limits
// (limits.md): depth 2 and 4 live children per goal.
TEST(WorkModel, tree_limits_missing_from_the_config_take_their_defaults) {
    WorkNode work;
    work.node().config().struct_size = offsetof(ayni_config_v1, goal_depth_max);
    work.node().config().goal_capacity = 16;
    work.run();
    const uint64_t root = work.create_goal();
    const uint64_t child = work.create_goal(root);
    const uint64_t grandchild = work.create_goal(child);
    ASSERT_NE(grandchild, 0U);
    EXPECT_EQ(work.goal(grandchild).depth, AYNI_GOAL_DEPTH_MAX_DEFAULT);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), grandchild)),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_DEPTH, AYNI_OBJECT_GOAL, grandchild);
    for (uint32_t children = 1; children < AYNI_GOAL_CHILDREN_MAX_DEFAULT; ++children) {
        ASSERT_NE(work.create_goal(root), 0U);
    }
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), root)),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CHILDREN, AYNI_OBJECT_GOAL, root);
}

// Capacity counts terminal goals; a refused goal allocates no id and takes no
// transition.
TEST(WorkModel, full_goal_table_refuses_without_side_effects) {
    WorkNode work;
    work.node().config().goal_capacity = 2;
    work.run();
    const uint64_t first = work.create_goal();
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, first).status, AYNI_OK);
    const Submitted second = work.create(WorkNode::create_body(work.instance(agent_a)));
    ASSERT_EQ(second.status, AYNI_OK);
    const int calls = work.rules().calls;
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a))),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CAPACITY);
    EXPECT_EQ(work.rules().calls, calls);

    const Submitted paused = work.change(AYNI_INTENT_GOAL_PAUSE, second.result.object.id);
    ASSERT_EQ(paused.status, AYNI_OK);
    EXPECT_EQ(paused.result.commit_seq, second.result.commit_seq + 1U);
    EXPECT_EQ(work.open_session(AYNI_CHANNEL_VOICE), second.result.object.id + 1U);
}

// C-ID-010: once this boot has no id left, new goals and sessions are
// refused, and nothing changes.
TEST(WorkModel, used_up_ids_refuse_goals_and_sessions) {
    WorkNode work;
    work.run();
    work.kernel()->work.ids.start(1, 0xFFFFFFFFU);
    const uint64_t last = work.create_goal();
    EXPECT_EQ(last, (uint64_t{1} << 32U) | 0xFFFFFFFFU);
    const int calls = work.rules().calls;
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a))),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_ID_EXHAUSTED);
    EXPECT_EQ(work.rules().calls, calls);
    const ayni_session_desc_v1 desc = session_desc();
    uint64_t session_id = 0;
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_CAPACITY_EXCEEDED);
    // Changing a goal needs no new id.
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, last).status, AYNI_OK);
}

// --- Changing goals -------------------------------------------------------

// C-LIFE-021, C-LIFE-022: pause stays on the goal; cancel reaches every live
// descendant in one transition.
TEST(WorkModel, pause_stays_on_the_goal_and_cancel_reaches_its_descendants) {
    WorkNode work;
    work.run();
    const uint64_t root = work.create_goal();
    const uint64_t child = work.create_goal(root);
    const uint64_t grandchild = work.create_goal(child);
    const uint64_t other = work.create_goal();

    const Submitted paused = work.change(AYNI_INTENT_GOAL_PAUSE, root);
    ASSERT_EQ(paused.status, AYNI_OK);
    EXPECT_EQ(paused.result.object.revision, 2U);
    EXPECT_EQ(work.goal(root).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, root).status, AYNI_OK);
    EXPECT_EQ(work.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(root).revision, 3U);

    const Submitted cancelled = work.change(AYNI_INTENT_GOAL_CANCEL, root);
    ASSERT_EQ(cancelled.status, AYNI_OK);
    EXPECT_EQ(cancelled.result.object.id, root);
    EXPECT_EQ(cancelled.result.object.revision, 4U);
    for (const uint64_t id : {root, child, grandchild}) {
        const ayni_goal_info_v1 goal = work.goal(id);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED) << id;
        EXPECT_EQ(goal.commit_seq, cancelled.result.commit_seq) << id;
        EXPECT_EQ(goal.cancel_revision, goal.revision) << id;
    }
    EXPECT_EQ(work.goal(child).revision, 2U);
    EXPECT_EQ(work.goal(other).lifecycle, AYNI_GOAL_ACTIVE);
    expect_refused(work.change(AYNI_INTENT_GOAL_CANCEL, child), AYNI_INVALID_STATE,
                   AYNI_REASON_GOAL_STATE, AYNI_OBJECT_GOAL, child);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, child).error.object.revision, 2U);
}

// C-COMMIT-012, E1.S9.user_edit_not_overwritten: a change based on an old
// revision is refused, and the refusal names the current one.
TEST(WorkModel, stale_revision_is_a_conflict_and_changes_nothing) {
    WorkNode work;
    work.run();
    const uint64_t goal = work.create_goal();
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal, 1).status, AYNI_OK);
    // Pausing a paused goal from revision 1: the revision is checked first.
    const Submitted stale = work.change(AYNI_INTENT_GOAL_PAUSE, goal, 1);
    expect_refused(stale, AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, AYNI_OBJECT_GOAL,
                   goal);
    EXPECT_EQ(stale.error.object.revision, 2U);
    expect_refused(work.change(AYNI_INTENT_GOAL_CANCEL, goal, 0), AYNI_REVISION_CONFLICT,
                   AYNI_REASON_GOAL_REVISION, AYNI_OBJECT_GOAL, goal);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(work.goal(goal).revision, 2U);
    expect_refused(work.change(AYNI_INTENT_GOAL_PAUSE, goal), AYNI_INVALID_STATE,
                   AYNI_REASON_GOAL_STATE, AYNI_OBJECT_GOAL, goal);
    expect_refused(work.change(AYNI_INTENT_GOAL_RESUME, goal + 1U, 1), AYNI_NOT_FOUND,
                   AYNI_REASON_GOAL_UNKNOWN, AYNI_OBJECT_GOAL, goal + 1U);
    // The refusals took no transition: the next change follows the pause.
    const uint64_t paused_at = work.goal(goal).commit_seq;
    const Submitted resumed = work.change(AYNI_INTENT_GOAL_RESUME, goal);
    ASSERT_EQ(resumed.status, AYNI_OK);
    EXPECT_EQ(resumed.result.commit_seq, paused_at + 1U);
}

// C-COMMIT-012 through the C ABI: a cancel that would raise a descendant
// past the largest revision is refused as a whole and takes no transition.
// White-box: reaching the largest revision would take 2^32 changes.
TEST(WorkModel, change_past_the_largest_revision_is_refused_and_changes_nothing) {
    WorkNode work;
    work.run();
    const uint64_t root = work.create_goal();
    const uint64_t child = work.create_goal(root);
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    ASSERT_TRUE(goals.find(child, index));
    const_cast<ayni::work::Goal &>(goals.at(index)).revision = std::numeric_limits<uint32_t>::max();

    expect_refused(work.change(AYNI_INTENT_GOAL_CANCEL, root), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_GOAL, root);
    EXPECT_EQ(work.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(root).revision, 1U);
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    expect_refused(work.change(AYNI_INTENT_GOAL_PAUSE, child), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_GOAL, child);
    const Submitted paused = work.change(AYNI_INTENT_GOAL_PAUSE, root);
    ASSERT_EQ(paused.status, AYNI_OK);
    EXPECT_EQ(paused.result.commit_seq, work.goal(child).commit_seq + 1U);
}

// C-POLICY-001: a validator must not call the kernel. As a fail-safe, every
// function that would change it returns INVALID_STATE, and the outer intent
// then completes as usual. With room for one goal, a nested create that got
// through would have overrun the goal table.
TEST(WorkModel, validator_cannot_change_the_kernel_while_it_runs) {
    WorkNode work;
    work.node().config().goal_capacity = 1;
    work.run();
    ayni_kernel_t *kernel = work.kernel();
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    const ayni_intent_v1 nested = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    std::vector<ayni_status_t> inside;
    work.rules().during = [&] {
        inside.push_back(ayni_submit_intent(kernel, &nested, nullptr, nullptr));
        const ayni_session_desc_v1 session = session_desc();
        uint64_t session_id = 0;
        inside.push_back(ayni_open_session(kernel, &session, &session_id));
        inside.push_back(ayni_close_session(kernel, work.session()));
        inside.push_back(ayni_step(kernel, 1, 1, nullptr));
        inside.push_back(ayni_begin_shutdown(kernel));
        inside.push_back(ayni_start(kernel));
        inside.push_back(ayni_destroy(kernel));
        const ayni_principal_desc_v1 principal = owner_desc(owner_id + 1U);
        inside.push_back(ayni_register_principal(kernel, &principal));
        const ayni_agent_desc_v1 agent = agent_desc(agent_b + 1U);
        inside.push_back(ayni_register_agent(kernel, &agent));
        const ayni_goal_type_v1 type = goal_type_desc("other", 1);
        inside.push_back(ayni_register_goal_type(kernel, &type));
    };

    const Submitted created = work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(inside, std::vector<ayni_status_t>(10, AYNI_INVALID_STATE));
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    // The session is still open, and the guard is gone once the call returns.
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, created.result.object.id).status, AYNI_OK);
    expect_refused(work.create(body), AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CAPACITY);
}

// --- Check order and arguments -------------------------------------------

TEST(WorkModel, intent_arguments_are_refused_before_anything_else) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    const ayni_intent_v1 valid = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    ayni_goal_target_v1 target{};
    target.struct_size = sizeof(target);

    EXPECT_EQ(ayni_submit_intent(work.kernel(), nullptr, nullptr, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_submit_intent(nullptr, &valid, nullptr, nullptr), AYNI_INVALID_ARGUMENT);

    std::vector<std::pair<const char *, ayni_intent_v1>> invalid;
    ayni_intent_v1 intent = valid;
    intent.struct_size -= 1U;
    invalid.emplace_back("short intent", intent);
    intent = valid;
    intent.kind = 0;
    invalid.emplace_back("kind 0", intent);
    intent = valid;
    intent.reserved = 1;
    invalid.emplace_back("reserved", intent);
    intent = valid;
    intent.body = nullptr;
    invalid.emplace_back("no body", intent);
    intent = valid;
    intent.expected_revision = 1;
    invalid.emplace_back("create from a revision", intent);
    static ayni_goal_create_v1 short_body;
    short_body = body;
    short_body.struct_size -= 1U;
    invalid.emplace_back("short body", work.intent(AYNI_INTENT_GOAL_CREATE, &short_body));
    static ayni_goal_create_v1 bad_type;
    bad_type = body;
    bad_type.goal_type_id = text("Reminder");
    invalid.emplace_back("goal type id", work.intent(AYNI_INTENT_GOAL_CREATE, &bad_type));
    static ayni_goal_create_v1 no_data;
    no_data = body;
    no_data.parameters = {nullptr, 2};
    invalid.emplace_back("parameters without data", work.intent(AYNI_INTENT_GOAL_CREATE, &no_data));
    static ayni_goal_target_v1 reserved_target;
    reserved_target = target;
    reserved_target.reserved = 1;
    invalid.emplace_back("target reserved", work.intent(AYNI_INTENT_GOAL_PAUSE, &reserved_target));

    for (auto &[name, bad] : invalid) {
        SCOPED_TRACE(name);
        // Even through a closed session and with a command id: arguments come first.
        bad.session_id = 999;
        bad.command_id = 5;
        expect_refused(work.submit(bad), AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE);
    }

    intent = valid;
    intent.struct_size += 8U;
    EXPECT_EQ(work.submit(intent).status, AYNI_UNSUPPORTED_VERSION);
    short_body = body;
    short_body.struct_size += 8U;
    EXPECT_EQ(work.submit(work.intent(AYNI_INTENT_GOAL_CREATE, &short_body)).status,
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(work.goal(work.create_goal()).revision, 1U); // nothing above was applied
}

TEST(WorkModel, output_struct_sizes_are_checked_and_a_bad_error_struct_is_not_written) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    const ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);

    ayni_error_v1 error{};
    error.struct_size = sizeof(error) - 1U;
    error.reason = 77;
    EXPECT_EQ(ayni_submit_intent(work.kernel(), &intent, nullptr, &error), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(error.reason, 77U);
    error.struct_size = sizeof(error) + 8U;
    EXPECT_EQ(ayni_submit_intent(work.kernel(), &intent, nullptr, &error),
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(error.reason, 77U);

    ayni_intent_result_v1 result{};
    result.struct_size = sizeof(result) - 1U;
    error.struct_size = sizeof(error);
    EXPECT_EQ(ayni_submit_intent(work.kernel(), &intent, &result, &error), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(error.status, AYNI_INVALID_ARGUMENT);
    result.struct_size = sizeof(result) + 8U;
    EXPECT_EQ(ayni_submit_intent(work.kernel(), &intent, &result, &error),
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(error.status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(work.create_goal(), (uint64_t{1} << 32U) | 4U); // no goal was created above
}

// Steps 2 to 4 of ayni_submit_intent: unsupported comes before NOT_READY,
// which comes before the session.
TEST(WorkModel, unsupported_intents_come_before_readiness_and_the_session) {
    WorkNode work;
    work.node().create();
    const ayni_goal_create_v1 body = WorkNode::create_body(1);
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);

    intent.command_id = 5;
    expect_refused(work.submit(intent), AYNI_UNSUPPORTED, AYNI_REASON_COMMAND_ID_UNSUPPORTED);
    intent.command_id = 0;
    for (const ayni_intent_kind_t kind : {5U, 12U, 13U, 0xFFFFFFFFU}) {
        intent.kind = kind;
        intent.body = nullptr; // not checked for a kind that is not implemented
        expect_refused(work.submit(intent), AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED);
    }
    intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.session_id = 999;
    expect_refused(work.submit(intent), AYNI_NOT_READY, AYNI_REASON_NONE);
}

// C-LIFE-004: outside RUNNING an intent is NOT_READY, is not queued and
// changes nothing.
TEST(WorkModel, intent_outside_running_is_not_ready) {
    for (const ayni_lifecycle_t state :
         {AYNI_LIFECYCLE_CONFIGURING, AYNI_LIFECYCLE_RECOVERING, AYNI_LIFECYCLE_RESTRICTED,
          AYNI_LIFECYCLE_DRAINING, AYNI_LIFECYCLE_STOPPED}) {
        SCOPED_TRACE(state);
        WorkNode work;
        work.node().enter(state);
        ayni_goal_target_v1 target{};
        target.struct_size = sizeof(target);
        target.goal_id = 1;
        const ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CANCEL, &target, 1);
        expect_refused(work.submit(intent), AYNI_NOT_READY, AYNI_REASON_NONE);
        EXPECT_EQ(work.node().lifecycle(), state);
    }
}

// lifecycles.md: DRAINING still answers queries and closes sessions, but
// takes no new session or intent, with the goals and sessions of RUNNING
// still in place.
TEST(WorkModel, draining_answers_queries_and_closes_sessions_but_takes_no_work) {
    WorkNode work;
    work.run();
    const uint64_t goal = work.create_goal();
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_DRAINING);

    expect_refused(work.change(AYNI_INTENT_GOAL_PAUSE, goal, 1), AYNI_NOT_READY, AYNI_REASON_NONE);
    const ayni_session_desc_v1 desc = session_desc();
    uint64_t session_id = 0;
    EXPECT_EQ(ayni_open_session(work.kernel(), &desc, &session_id), AYNI_NOT_READY);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.agent(agent_a).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(ayni_close_session(work.kernel(), work.session()), AYNI_OK);
    EXPECT_EQ(ayni_close_session(work.kernel(), work.session()), AYNI_NOT_FOUND);
}

// --- Reboots --------------------------------------------------------------

// C-COMMIT-016: transition_id and commit_seq place a transition in the log,
// and a later boot reuses the places of transitions that were never durable.
// One boot later a new goal takes the place of an unstored one; two boots
// later durable_seq covers the first goal's commit_seq although the goal is
// gone. So both are compared only with durable_seq of their own boot.
TEST(WorkModel, commit_seq_of_unstored_work_is_reused_by_later_boots) {
    WorkNode work;
    work.run();
    const ayni_goal_info_v1 first = work.goal(work.create_goal());
    const ayni_goal_info_v1 second = work.goal(work.create_goal());
    EXPECT_GT(first.commit_seq, work.node().info().durable_seq);

    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.session_id = work.open_session();
    const Submitted created = work.submit(intent);
    ASSERT_EQ(created.status, AYNI_OK);
    const ayni_goal_info_v1 reused = work.goal(created.result.object.id);
    EXPECT_NE(reused.goal_id, second.goal_id);
    EXPECT_EQ(reused.transition_id, second.transition_id);
    EXPECT_EQ(reused.commit_seq, second.commit_seq);

    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_GE(work.node().info().durable_seq, first.commit_seq);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), first.goal_id, &info), AYNI_NOT_FOUND);
}

// C-ID-001, C-ID-010: a later boot never assigns an id an earlier boot
// assigned. Until the log stores them (MA-010), goals, instances and sessions
// do not survive the reboot.
TEST(WorkModel, a_reboot_assigns_new_ids_and_forgets_unstored_work) {
    WorkNode work;
    work.run();
    const uint64_t first_instance = work.instance(agent_a);
    const uint64_t first_session = work.session();
    const uint64_t first_goal = work.create_goal();

    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), first_goal, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(work.node().info().durable_seq, 2U); // two boot batches, nothing else
    const uint64_t second_instance = work.instance(agent_a);
    EXPECT_EQ(second_instance >> 32U, 2U);
    EXPECT_NE(second_instance, first_instance);

    const ayni_goal_create_v1 body = WorkNode::create_body(second_instance);
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.session_id = first_session;
    expect_refused(work.submit(intent), AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                   AYNI_OBJECT_SESSION, first_session);

    const uint64_t session = work.open_session();
    intent.session_id = session;
    const Submitted second = work.submit(intent);
    ASSERT_EQ(second.status, AYNI_OK);
    for (const uint64_t earlier : {first_instance, first_session, first_goal}) {
        EXPECT_NE(second.result.object.id, earlier);
        EXPECT_NE(session, earlier);
    }
}

// --- Return values per state ---------------------------------------------

enum class Call {
    register_principal,
    register_agent,
    register_goal_type,
    open_session,
    close_session,
    submit_intent,
    get_agent,
    get_goal,
};

ayni_status_t invoke(ayni_kernel_t *kernel, Call call) {
    switch (call) {
    case Call::register_principal: {
        const ayni_principal_desc_v1 desc = owner_desc();
        return ayni_register_principal(kernel, &desc);
    }
    case Call::register_agent: {
        const ayni_agent_desc_v1 desc = agent_desc(agent_a);
        return ayni_register_agent(kernel, &desc);
    }
    case Call::register_goal_type: {
        const ayni_goal_type_v1 desc = goal_type_desc("reminder", 1);
        return ayni_register_goal_type(kernel, &desc);
    }
    case Call::open_session: {
        const ayni_session_desc_v1 desc = session_desc();
        uint64_t session_id = 0;
        return ayni_open_session(kernel, &desc, &session_id);
    }
    case Call::close_session:
        return ayni_close_session(kernel, 12345);
    case Call::submit_intent: {
        ayni_goal_target_v1 target{};
        target.struct_size = sizeof(target);
        target.goal_id = 1;
        ayni_intent_v1 intent{};
        intent.struct_size = sizeof(intent);
        intent.kind = AYNI_INTENT_GOAL_PAUSE;
        intent.expected_revision = 1;
        intent.body = &target;
        return ayni_submit_intent(kernel, &intent, nullptr, nullptr);
    }
    case Call::get_agent: {
        ayni_agent_info_v1 info{};
        info.struct_size = sizeof(info);
        return ayni_get_agent(kernel, agent_a, &info);
    }
    case Call::get_goal: {
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        return ayni_get_goal(kernel, 1, &info);
    }
    }
    return AYNI_INTERNAL;
}

// contracts/domain/lifecycles.md: the work model's rows of the return-value
// table. Registration finds an empty catalog, and the other calls find the
// owner and agent_a registered.
TEST(WorkModel, return_values_follow_the_state_table) {
    constexpr ayni_status_t ok = AYNI_OK;
    constexpr ayni_status_t bad = AYNI_INVALID_STATE;
    constexpr ayni_status_t not_ready = AYNI_NOT_READY;
    constexpr ayni_status_t none = AYNI_NOT_FOUND;
    const ayni_lifecycle_t states[] = {
        AYNI_LIFECYCLE_CONFIGURING, AYNI_LIFECYCLE_RECOVERING, AYNI_LIFECYCLE_RUNNING,
        AYNI_LIFECYCLE_RESTRICTED,  AYNI_LIFECYCLE_DRAINING,   AYNI_LIFECYCLE_STOPPED,
    };
    const struct {
        Call call;
        const char *name;
        bool registered;
        std::array<ayni_status_t, 6> expected;
    } table[] = {
        {Call::register_principal, "ayni_register_principal", false, {ok, bad, bad, bad, bad, bad}},
        {Call::register_agent, "ayni_register_agent", false, {ok, bad, bad, bad, bad, bad}},
        {Call::register_goal_type, "ayni_register_goal_type", false, {ok, bad, bad, bad, bad, bad}},
        {Call::open_session,
         "ayni_open_session",
         true,
         {not_ready, not_ready, ok, not_ready, not_ready, not_ready}},
        {Call::close_session, "ayni_close_session", true, {bad, none, none, none, none, bad}},
        {Call::submit_intent,
         "ayni_submit_intent",
         true,
         {not_ready, not_ready, AYNI_DENIED, not_ready, not_ready, not_ready}},
        {Call::get_agent, "ayni_get_agent", true, {bad, ok, ok, ok, ok, bad}},
        {Call::get_goal, "ayni_get_goal", true, {bad, none, none, none, none, bad}},
    };
    for (const auto &row : table) {
        for (std::size_t column = 0; column < 6; ++column) {
            SCOPED_TRACE(::testing::Message() << row.name << " in state " << states[column]);
            Node node;
            if (row.registered) {
                node.on_create([](ayni_kernel_t *kernel) {
                    const ayni_principal_desc_v1 owner = owner_desc();
                    const ayni_agent_desc_v1 agent = agent_desc(agent_a);
                    ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
                    ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
                });
            }
            node.enter(states[column]);
            EXPECT_EQ(invoke(node.kernel(), row.call), row.expected[column]);
            EXPECT_EQ(node.lifecycle(), states[column]);
        }
    }
}

} // namespace
