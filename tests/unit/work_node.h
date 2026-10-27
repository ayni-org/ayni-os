// The work model fixture shared by the tests that drive it through the C
// ABI: an owner, two agents, a strategy and three goal types registered on
// every kernel, one session, and helpers that submit intents and query what
// they changed.

#ifndef AYNI_TESTS_UNIT_WORK_NODE_H
#define AYNI_TESTS_UNIT_WORK_NODE_H

#include "kernel_node.h"

#include <ayni/ayni.h>
#include <ayni/host_authority.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
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

// What a strategy saw of a task, copied out of the views that end with the
// decide call.
struct Seen {
    ayni_task_snapshot_v1 snapshot;
    std::string goal_type_id;
    std::vector<uint8_t> parameters;
    std::string checkpoint_strategy_id;
    std::vector<uint8_t> checkpoint;
    std::vector<std::string> steps;
};

inline std::string string_of(ayni_string_v1 view) {
    return view.size > 0U ? std::string(view.data, view.size) : std::string();
}

inline std::vector<uint8_t> bytes_of(ayni_bytes_v1 view) {
    return view.size > 0U ? std::vector<uint8_t>(view.data, view.data + view.size)
                          : std::vector<uint8_t>();
}

inline Seen seen_of(const ayni_task_snapshot_v1 &snapshot) {
    Seen seen{snapshot,
              string_of(snapshot.goal_type_id),
              bytes_of(snapshot.goal_parameters),
              string_of(snapshot.checkpoint.strategy_id),
              bytes_of(snapshot.checkpoint.state),
              {}};
    for (uint32_t step = 0; step < snapshot.step_count; ++step) {
        seen.steps.push_back(string_of(snapshot.steps[step].step_key));
    }
    return seen;
}

// An intent of kind for expected_revision, with step_key for
// REPORT_PROGRESS.
inline ayni_policy_intent_v1 policy_intent(ayni_policy_intent_kind_t kind,
                                           uint32_t expected_revision,
                                           const std::string &step_key = std::string()) {
    ayni_policy_intent_v1 intent{};
    intent.struct_size = sizeof(intent);
    intent.kind = kind;
    intent.expected_revision = expected_revision;
    intent.step_key_size = static_cast<uint32_t>(step_key.size());
    std::memcpy(intent.step_key, step_key.data(),
                std::min(step_key.size(), sizeof(intent.step_key)));
    return intent;
}

inline ayni_policy_intent_v1 progress_intent(const std::string &step_key,
                                             uint32_t expected_revision) {
    return policy_intent(AYNI_POLICY_REPORT_PROGRESS, expected_revision, step_key);
}

// Writes one intent of kind for the snapshot's revision into the output.
inline ayni_status_t report(ayni_policy_intent_kind_t kind, const ayni_task_snapshot_v1 &snapshot,
                            ayni_policy_output_v1 &output) {
    output.intents[0] = policy_intent(kind, snapshot.revision);
    output.intent_count = 1;
    return AYNI_OK;
}

// An answer that cannot decide, so the strategy holds every task it decides
// PAUSED with AYNI_UNSUPPORTED: the task stays live without advancing.
inline ayni_status_t cannot_decide(const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &) {
    return AYNI_UNSUPPORTED;
}

// The strategy of the reminder and timer goal types. It records what it saw
// and answers as the test scripts it; by default it reports completion, so
// a task ends in the step after it became READY.
struct ScriptedStrategy {
    using Answer =
        std::function<ayni_status_t(const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &)>;

    static ayni_status_t decide(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                                ayni_policy_output_v1 *output) {
        auto &self = *static_cast<ScriptedStrategy *>(ctx);
        self.seen.push_back(seen_of(*snapshot));
        self.outputs.push_back(*output);
        self.output_checkpoints.emplace_back(output->checkpoint,
                                             output->checkpoint + output->checkpoint_capacity);
        if (self.during) {
            // Runs once, from a copy, so that it may call the kernel freely.
            const std::function<void()> during = std::move(self.during);
            self.during = nullptr;
            during();
        }
        if (self.answer) {
            return self.answer(*snapshot, *output);
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, *snapshot, *output);
    }

    std::vector<Seen> seen;
    std::vector<ayni_policy_output_v1> outputs; // as each decide call got it
    // The checkpoint_capacity bytes of each output's checkpoint as decide
    // got them.
    std::vector<std::vector<uint8_t>> output_checkpoints;
    Answer answer;                // null: report completion
    std::function<void()> during; // runs once inside the next decision
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
    AYNI_TRIGGER_BIT(AYNI_TRIGGER_DELAY) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL) |
    AYNI_TRIGGER_BIT(AYNI_TRIGGER_LOCAL_INPUT);

constexpr const char *strategy_id = "scripted";

// Occurrences wait without limit unless a test asks for one (ADR-0020).
// Types whose goals may have tasks name the scripted strategy.
inline ayni_goal_type_v1
goal_type_desc(const char *id, uint32_t version, ayni_goal_validate_fn validate = nullptr,
               void *ctx = nullptr, uint32_t trigger_kinds = any_trigger,
               uint64_t occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED) {
    ayni_goal_type_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.version = version;
    desc.goal_type_id = text(id);
    desc.ctx = ctx;
    desc.validate = validate;
    desc.trigger_kinds = trigger_kinds;
    desc.occurrence_wait_max_us = occurrence_wait_max_us;
    if (trigger_kinds != AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE)) {
        desc.strategy_id = text(strategy_id);
    }
    return desc;
}

inline ayni_strategy_v1 strategy_desc(const char *id, uint32_t version, ayni_decide_fn decide,
                                      void *ctx, uint32_t max_intents_per_call = 1,
                                      uint32_t max_checkpoint_bytes = 0) {
    ayni_strategy_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.version = version;
    desc.strategy_id = text(id);
    desc.ctx = ctx;
    desc.decide = decide;
    desc.max_intents_per_call = max_intents_per_call;
    desc.intent_size = sizeof(ayni_policy_intent_v1);
    desc.max_checkpoint_bytes = max_checkpoint_bytes;
    return desc;
}

inline ayni_session_desc_v1 session_desc(ayni_channel_t channel = AYNI_CHANNEL_MANAGEMENT,
                                         uint64_t principal_id = owner_id) {
    ayni_session_desc_v1 desc{};
    desc.struct_size = sizeof(desc);
    desc.channel = channel;
    desc.principal_id = principal_id;
    desc.binding_revision = owner_binding;
    return desc;
}

struct Submitted {
    ayni_status_t status;
    ayni_intent_result_v1 result;
    ayni_error_v1 error;
};

// A node with an owner, two agents, the scripted strategy and three goal
// types registered on every kernel it creates: "reminder" with a validator
// and "timer" without, both taking every trigger kind and decided by the
// strategy, and "folder", whose goals derive no tasks. Occurrences of
// "timer" goals wait without limit unless a test sets timer_wait_max_us
// before the kernel is created; the strategy takes max_intents_per_call
// intents and returns private checkpoints of up to max_checkpoint_bytes,
// likewise.
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
                goal_type_desc("timer", 1, nullptr, nullptr, timer_trigger_kinds,
                               timer_wait_max_us),
                goal_type_desc("folder", 1, folder_validate, nullptr,
                               AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE)),
            };
            for (const ayni_goal_type_v1 &type : types) {
                ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            }
            const ayni_strategy_v1 strategy =
                strategy_desc(strategy_id, 1, &ScriptedStrategy::decide, &strategy_,
                              max_intents_per_call, max_checkpoint_bytes);
            ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
            if (additional_registration) {
                additional_registration(kernel);
            }
        });
    }

    Node &node() { return node_; }
    ayni_kernel_t *kernel() { return node_.kernel(); }
    std::function<void(ayni_kernel_t *)>
        additional_registration;                // Optional extra public test catalog.
    uint32_t timer_trigger_kinds = any_trigger; // Tests may explicitly enable new trigger families.
    uint64_t timer_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    // Without a validator, "folder" goals take no parameters.
    ayni_goal_validate_fn folder_validate = nullptr;
    uint32_t max_intents_per_call = 1;
    uint32_t max_checkpoint_bytes = 0;
    ReminderRules &rules() { return rules_; }
    ScriptedStrategy &strategy() { return strategy_; }

    // Into RUNNING with one management session open.
    void run() {
        node_.run();
        session_ = open_session();
    }

    uint64_t session() const { return session_; }

    // After a restart: sessions are never stored, so the node opens another.
    void reopen_session() { session_ = open_session(); }

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
    ScriptedStrategy strategy_;
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
