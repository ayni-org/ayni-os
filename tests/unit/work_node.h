// The work model fixture shared by the tests that drive it through the C
// ABI: an owner, two agents and three goal types registered on every kernel,
// one session, and helpers that submit intents and query what they changed.

#ifndef AYNI_TESTS_UNIT_WORK_NODE_H
#define AYNI_TESTS_UNIT_WORK_NODE_H

#include "kernel_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace ayni::test {

constexpr uint64_t owner_id = 7;
constexpr uint32_t owner_binding = 3;
constexpr uint64_t agent_a = 100;
constexpr uint64_t agent_b = 200;

inline ayni_string_v1 text(const char *value) {
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

inline ayni_principal_desc_v1 owner_desc(uint64_t principal_id = owner_id) {
    ayni_principal_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.kind = AYNI_PRINCIPAL_OWNER;
    desc.principal_id = principal_id;
    desc.binding_revision = owner_binding;
    return desc;
}

inline ayni_agent_desc_v1 agent_desc(uint64_t agent_id) {
    ayni_agent_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.agent_id = agent_id;
    return desc;
}

constexpr uint32_t any_trigger =
    AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) |
    AYNI_TRIGGER_BIT(AYNI_TRIGGER_DELAY) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL);

inline ayni_goal_type_v1 goal_type_desc(const char *id, uint32_t version,
                                        ayni_goal_validate_fn validate = nullptr,
                                        void *ctx = nullptr, uint32_t trigger_kinds = any_trigger) {
    ayni_goal_type_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.version = version;
    desc.goal_type_id = text(id);
    desc.ctx = ctx;
    desc.validate = validate;
    desc.trigger_kinds = trigger_kinds;
    return desc;
}

inline ayni_session_desc_v1 session_desc(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT,
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

// A node with an owner, two agents and three goal types registered on every
// kernel it creates: "reminder" with a validator and "timer" without, both
// taking every trigger kind, and "folder", whose goals derive no tasks.
class WorkNode {
  public:
    WorkNode() {
        node_.config().goal_type_capacity = 3;
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
                goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE)),
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
                                           ayni_bytes_v1 parameters = {nullptr, 0},
                                           ayni_trigger_kind_t trigger = AYNI_TRIGGER_NONE) {
        ayni_goal_create_v1 body{};
        body.struct_size = sizeof(body);
        body.goal_type_version = version;
        body.goal_type_id = text(type);
        body.owner_instance_id = owner_instance_id;
        body.parent_goal_id = parent_goal_id;
        body.parameters = parameters;
        body.trigger = trigger;
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

    ayni_task_info_v1 task(uint64_t task_id) {
        ayni_task_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_task(kernel(), task_id, &info), AYNI_OK);
        return info;
    }

    Submitted cancel_task(uint64_t task_id, uint32_t expected_revision) {
        ayni_task_target_v1 body{};
        body.struct_size = sizeof(body);
        body.task_id = task_id;
        return submit(intent(AYNI_INTENT_TASK_CANCEL, &body, expected_revision));
    }

    // Cancels the task at its current revision.
    Submitted cancel_task(uint64_t task_id) { return cancel_task(task_id, task(task_id).revision); }

    Submitted change_instance(ayni_intent_kind_t kind, uint64_t instance_id,
                              uint32_t expected_revision) {
        ayni_instance_target_v1 body{};
        body.struct_size = sizeof(body);
        body.instance_id = instance_id;
        return submit(intent(kind, &body, expected_revision));
    }

    // Changes the agent's instance at its current revision.
    Submitted change_agent(ayni_intent_kind_t kind, uint64_t agent_id) {
        const ayni_agent_info_v1 info = agent(agent_id);
        return change_instance(kind, info.instance_id, info.revision);
    }

  private:
    ReminderRules rules_;
    Node node_;
    uint64_t session_ = 0;
};

inline void expect_refused(const Submitted &submitted, ayni_status_t status, ayni_reason_t reason,
                           ayni_object_kind_t kind = 0, uint64_t id = 0) {
    EXPECT_EQ(submitted.status, status);
    EXPECT_EQ(submitted.error.reason, reason);
    EXPECT_EQ(submitted.error.object.kind, kind);
    EXPECT_EQ(submitted.error.object.id, id);
    EXPECT_EQ(submitted.error.reserved, 0U);
}

} // namespace ayni::test

#endif // AYNI_TESTS_UNIT_WORK_NODE_H
