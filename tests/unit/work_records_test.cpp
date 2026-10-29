// Record bodies of the work log (contracts/storage/records.md, ADR-0026):
// each codec of types 2 to 9 round trips, encodes fixed-width little-endian
// fields and length-prefixed strings (C-VER-023), keeps no time of the boot
// that wrote it (C-STORE-014), and each reader refuses a body its writer
// never produces.

#include "catalog/catalog_id.h"
#include "foundation/little_endian.h"
#include "kernel/service/command/command_records.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using ayni::catalog::CatalogId;
using ayni::foundation::load_le32;
using ayni::foundation::load_le64;
using ayni::kernel::CommandMapping;
using ayni::work::AgentInstance;
using ayni::work::Goal;
using ayni::work::GoalRecord;
using ayni::work::GoalStateRecord;
using ayni::work::InstanceRecord;
using ayni::work::no_time;
using ayni::work::Task;
using ayni::work::TaskCheckpointRecord;
using ayni::work::TaskRecord;
using ayni::work::TaskStateRecord;
using ayni::work::TaskTable;

constexpr uint64_t goal_id = 0x1'0000'0005ULL;
constexpr uint64_t parent_goal_id = 0x1'0000'0004ULL;
constexpr uint64_t owner = 0x1'0000'0002ULL;
constexpr uint64_t replaced_goal_id = 0x1'0000'0003ULL;
constexpr uint64_t principal = 7;
constexpr uint64_t task_id = 0x1'0000'0009ULL;
constexpr const char *long_id = "abcdefghijklmnopqrstuvwxyz.01234"; // 32 bytes
constexpr const char *too_long_id = "abcdefghijklmnopqrstuvwxyz.012345";

CatalogId catalog_id(const std::string &text) {
    CatalogId id;
    EXPECT_TRUE(id.assign({text.data(), static_cast<uint32_t>(text.size())}));
    return id;
}

std::string string_of(ayni_string_v1 view) {
    return view.size > 0U ? std::string(view.data, view.size) : std::string();
}

std::string string_of(const CatalogId &id) {
    return std::string(id.data(), id.size());
}

std::string string_of(ayni_bytes_v1 view) {
    return view.size > 0U ? std::string(reinterpret_cast<const char *>(view.data), view.size)
                          : std::string();
}

ayni_bytes_v1 bytes_of(const std::string &text) {
    return {reinterpret_cast<const uint8_t *>(text.data()), static_cast<uint32_t>(text.size())};
}

// A body written field by field, as C-VER-023 lays fields out, to compare
// with what a writer produced or to break one field of it.
class Body {
  public:
    Body &u32(uint32_t value) {
        for (uint32_t shift = 0; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<uint8_t>(value >> shift));
        }
        return *this;
    }
    Body &u64(uint64_t value) {
        for (uint32_t shift = 0; shift < 64U; shift += 8U) {
            bytes_.push_back(static_cast<uint8_t>(value >> shift));
        }
        return *this;
    }
    // A string with its uint32 length prefix.
    Body &text(const std::string &value) {
        return u32(static_cast<uint32_t>(value.size())).raw(value);
    }
    // Bytes without a prefix.
    Body &raw(const std::string &value) {
        for (const char byte : value) {
            bytes_.push_back(static_cast<uint8_t>(byte));
        }
        return *this;
    }
    Body &raw(const std::vector<uint8_t> &value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return *this;
    }

    const uint8_t *data() const { return bytes_.data(); }
    uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
    const std::vector<uint8_t> &bytes() const { return bytes_; }

  private:
    std::vector<uint8_t> bytes_;
};

// A buffer of size bytes plus one, for a reader to see too long a body.
std::vector<uint8_t> buffer_for(uint32_t size) {
    return std::vector<uint8_t>(size + 1U, 0);
}

// A fixed-size reader accepts a body of its size and no other.
template <typename Read> void expect_size_only(uint32_t size, Read read) {
    std::vector<uint8_t> body = buffer_for(size);
    if (size == ayni::work::goal_state_record_size) {
        ayni::foundation::store_le32(body.data() + 48, 1);
    } else if (size == ayni::work::task_state_record_size) {
        ayni::foundation::store_le32(body.data() + 36, 8);
        ayni::foundation::store_le32(body.data() + 84, 1);
    }
    EXPECT_TRUE(read(body.data(), size));
    for (uint32_t shorter = 0; shorter < size; ++shorter) {
        EXPECT_FALSE(read(body.data(), shorter)) << shorter;
    }
    EXPECT_FALSE(read(body.data(), size + 1U));
}

// A reader of a body with strings refuses the body cut anywhere or followed
// by another byte.
template <typename Read> void expect_whole_body_only(const Body &body, Read read) {
    std::vector<uint8_t> bytes = body.bytes();
    EXPECT_TRUE(read(bytes.data(), body.size()));
    for (uint32_t shorter = 0; shorter < body.size(); ++shorter) {
        EXPECT_FALSE(read(bytes.data(), shorter)) << shorter;
    }
    bytes.push_back(0);
    EXPECT_FALSE(read(bytes.data(), body.size() + 1U));
}

// The fields of a GOAL record that the tests vary.
struct GoalFields {
    std::string goal_type_id = "reminder";
    ayni_trigger_kind_t trigger = AYNI_TRIGGER_INTERVAL;
    uint64_t delay_us = 30;
    uint64_t period_us = 1'000'000;
    std::string trigger_source;
    std::string parameters = "ab";
};

// The GOAL body of the goal goal_of(fields), field by field.
Body goal_body(const GoalFields &fields) {
    Body body;
    body.u64(goal_id).u64(parent_goal_id).u64(owner).u64(replaced_goal_id).u64(principal);
    body.u32(AYNI_ORIGIN_RULE).u32(AYNI_CHANNEL_MANAGEMENT).u32(3).u32(1).u32(2);
    body.text(fields.goal_type_id);
    body.u32(fields.trigger).u64(fields.delay_us).u64(fields.period_us);
    body.text(fields.trigger_source);
    body.u64(900).u64(60);
    body.text(fields.parameters).u32(0).u64(0).u32(0).u32(0).u32(0);
    return body;
}

// A goal as the table holds it. Its times are this boot's: created at
// created_us, its next occurrence and expiry both after that.
Goal goal_of(const GoalFields &fields, uint64_t created_us = 100) {
    Goal goal{};
    goal.goal_id = goal_id;
    goal.parent_goal_id = parent_goal_id;
    goal.owner_instance_id = owner;
    goal.replaced_goal_id = replaced_goal_id;
    goal.created = {40, 40};
    goal.commit_seq = 30;
    goal.origin = {AYNI_ORIGIN_RULE, AYNI_CHANNEL_MANAGEMENT, principal, 3};
    goal.depth = 1;
    goal.goal_type_version = 2;
    goal.goal_type_id = catalog_id(fields.goal_type_id);
    goal.trigger = {fields.trigger, fields.delay_us, fields.period_us};
    if (!fields.trigger_source.empty()) {
        goal.trigger_source = catalog_id(fields.trigger_source);
    }
    goal.expiry_after_us = 900;
    goal.expires_us = created_us + 900U;
    goal.occurrence_wait_max_us = 60;
    goal.cursor = {0, created_us + fields.delay_us};
    goal.parameter_size = static_cast<uint32_t>(fields.parameters.size());
    goal.lifecycle = AYNI_GOAL_ACTIVE;
    goal.revision = 1;
    return goal;
}

std::vector<uint8_t> written_goal(const Goal &goal, const std::string &parameters,
                                  ayni::work::GoalRecordOptions options = {}) {
    std::vector<uint8_t> body(ayni::work::goal_record_size(goal));
    ayni::work::write_goal_record(goal, bytes_of(parameters), body.data(), options);
    return body;
}

bool reads_goal(const Body &body) {
    GoalRecord record{};
    return ayni::work::read_goal_record(body.data(), body.size(), record);
}

std::vector<uint8_t> written_goal_state(const Goal &goal) {
    std::vector<uint8_t> body(ayni::work::goal_state_record_size);
    ayni::work::write_goal_state_record(goal, body.data(), ayni::work::GoalRecordChange::state);
    return body;
}

Task task_of() {
    Task task{};
    task.task_id = task_id;
    task.goal_id = goal_id;
    task.owner_instance_id = owner;
    task.replaced_task_id = 0x1'0000'0008ULL;
    task.created_transition_id = 41;
    task.commit_seq = 31;
    task.due_us = 1234;
    task.occurrence = 9;
    task.occurrence_count = 4;
    task.revision = 6;
    task.lifecycle = AYNI_TASK_READY;
    return task;
}

std::vector<uint8_t> written_task(const Task &task) {
    std::vector<uint8_t> body(ayni::work::task_record_size);
    ayni::work::write_task_record(task, body.data(), false);
    return body;
}

// The keys a TASK_STEPS record holds, with their positions.
using Steps = std::vector<std::pair<uint32_t, std::string>>;

bool read_steps(const uint8_t *body, uint32_t size, uint64_t &out_task_id, Steps &out_steps) {
    out_steps.clear();
    return ayni::work::read_task_steps_record(body, size, out_task_id,
                                              [&out_steps](uint32_t step, ayni_string_v1 key) {
                                                  out_steps.emplace_back(step, string_of(key));
                                                  return true;
                                              });
}

bool reads_steps(const Body &body) {
    uint64_t read_id = 0;
    Steps steps;
    return read_steps(body.data(), body.size(), read_id, steps);
}

bool reads_checkpoint(const Body &body) {
    TaskCheckpointRecord record{};
    return ayni::work::read_task_checkpoint_record(body.data(), body.size(), record);
}

class Tasks {
  public:
    explicit Tasks(uint32_t step_capacity)
        : limits_{2, step_capacity, 8},
          storage_(TaskTable::storage_size(limits_) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), limits_) {}

    TaskTable &table() { return table_; }

  private:
    TaskTable::Limits limits_;
    std::vector<uint64_t> storage_;
    TaskTable table_;
};

// INSTANCE: the instance as its last change left it; the stamp is the
// batch's, so the body holds ids, revision and lifecycle only.
TEST(WorkRecords, instance_record_round_trips_with_its_field_layout) {
    const AgentInstance instance{
        100, 0x1'0000'0001ULL, {77, 66}, 5, AYNI_AGENT_PAUSED, AYNI_REASON_NONE, true};
    std::vector<uint8_t> body = buffer_for(ayni::work::instance_record_size);
    ayni::work::write_instance_record(instance, body.data());
    EXPECT_EQ(ayni::work::instance_record_size, 24U);
    EXPECT_EQ(load_le64(body.data()), 100U);
    EXPECT_EQ(load_le64(body.data() + 8), 0x1'0000'0001ULL);
    EXPECT_EQ(load_le32(body.data() + 16), 5U);
    EXPECT_EQ(load_le32(body.data() + 20), AYNI_AGENT_PAUSED);

    InstanceRecord record{};
    ASSERT_TRUE(ayni::work::read_instance_record(body.data(), 24, record));
    EXPECT_EQ(record.agent_id, 100U);
    EXPECT_EQ(record.instance_id, 0x1'0000'0001ULL);
    EXPECT_EQ(record.revision, 5U);
    EXPECT_EQ(record.lifecycle, AYNI_AGENT_PAUSED);
}

TEST(WorkRecords, instance_record_of_another_size_is_refused) {
    expect_size_only(ayni::work::instance_record_size, [](const uint8_t *body, uint32_t size) {
        InstanceRecord record{};
        return ayni::work::read_instance_record(body, size, record);
    });
}

// GOAL: what the goal was created with, its goal type by id and version for
// a later boot to resolve, durations rather than times, and its parameters,
// which the record keeps in place.
// ADR-0047/records.md: the appended profile preserves the old prefix and has explicit versions.
TEST(WorkRecords, nondefault_goal_profile_has_explicit_journal_and_snapshot_encoding) {
    GoalFields fields;
    fields.period_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US;
    auto goal = goal_of(fields);
    const auto ordinary = written_goal(goal, fields.parameters);
    goal.work_profile = {AYNI_WORK_BACKGROUND, AYNI_BACKGROUND_PERIOD_DEFAULT_US};
    const auto body = written_goal(goal, fields.parameters, {true, false});
    ASSERT_EQ(body.size(), ordinary.size());
    constexpr auto tail = ayni::work::goal_work_profile_size + ayni::work::goal_group_tail_size +
                          ayni::work::goal_options_size;
    EXPECT_EQ(load_le32(body.data() + body.size() - tail), AYNI_WORK_BACKGROUND);
    GoalRecord record{};
    ASSERT_TRUE(
        ayni::work::read_goal_record(body.data(), static_cast<uint32_t>(body.size()), record));
    EXPECT_TRUE(record.goal.work_profile == goal.work_profile);
    EXPECT_EQ(string_of(record.goal.parameters), fields.parameters);
    EXPECT_TRUE(record.authority_required);
}

TEST(WorkRecords, goal_record_round_trips_with_its_strings_and_parameters) {
    GoalFields local;
    local.goal_type_id = long_id;
    local.trigger = AYNI_TRIGGER_LOCAL_INPUT;
    local.delay_us = 0;
    local.period_us = 0;
    local.trigger_source = long_id;
    local.parameters = std::string(40, '\x7F');
    GoalFields empty_parameters;
    empty_parameters.parameters.clear();
    for (const GoalFields &fields : {GoalFields{}, local, empty_parameters}) {
        const Goal goal = goal_of(fields);
        const std::vector<uint8_t> body = written_goal(goal, fields.parameters);
        EXPECT_EQ(body.size(), ayni::work::goal_record_fixed_size + fields.goal_type_id.size() +
                                   fields.trigger_source.size() + fields.parameters.size());

        GoalRecord record{};
        ASSERT_TRUE(
            ayni::work::read_goal_record(body.data(), static_cast<uint32_t>(body.size()), record));
        EXPECT_EQ(record.goal_id, goal_id);
        EXPECT_EQ(record.replaced_goal_id, replaced_goal_id);
        EXPECT_EQ(record.depth, 1U);
        EXPECT_EQ(record.goal.parent_goal_id, parent_goal_id);
        EXPECT_EQ(record.goal.owner_instance_id, owner);
        EXPECT_EQ(record.goal.origin.kind, AYNI_ORIGIN_RULE);
        EXPECT_EQ(record.goal.origin.channel, AYNI_CHANNEL_MANAGEMENT);
        EXPECT_EQ(record.goal.origin.principal_id, principal);
        EXPECT_EQ(record.goal.origin.binding_revision, 3U);
        EXPECT_EQ(string_of(record.goal.goal_type_id), fields.goal_type_id);
        EXPECT_EQ(record.goal.goal_type_version, 2U);
        EXPECT_EQ(record.goal.trigger.kind, fields.trigger);
        EXPECT_EQ(record.goal.trigger.delay_us, fields.delay_us);
        EXPECT_EQ(record.goal.trigger.period_us, fields.period_us);
        EXPECT_EQ(string_of(record.goal.trigger_source), fields.trigger_source);
        EXPECT_EQ(record.goal.expiry_us, 900U);
        EXPECT_EQ(record.goal.occurrence_wait_max_us, 60U);
        EXPECT_EQ(string_of(record.goal.parameters), fields.parameters);
        if (!fields.parameters.empty()) {
            EXPECT_GE(record.goal.parameters.data, body.data());
            EXPECT_LE(record.goal.parameters.data + record.goal.parameters.size,
                      body.data() + body.size());
        }
        // This boot's: resolved and set at recovery.
        EXPECT_EQ(record.goal.goal_type_index, 0U);
        EXPECT_EQ(record.goal.created_us, 0U);
    }
}

// C-VER-023: fixed-width little-endian fields in a fixed order, each string
// after its uint32 length.
TEST(WorkRecords, goal_record_is_encoded_field_by_field) {
    GoalFields local;
    local.trigger = AYNI_TRIGGER_LOCAL_INPUT;
    local.delay_us = 0;
    local.period_us = 0;
    local.trigger_source = "button.top";
    for (const GoalFields &fields : {GoalFields{}, local}) {
        EXPECT_EQ(written_goal(goal_of(fields), fields.parameters), goal_body(fields).bytes());
    }
}

TEST(WorkRecords, goal_record_cut_short_or_followed_by_more_is_refused) {
    GoalFields local;
    local.trigger = AYNI_TRIGGER_LOCAL_INPUT;
    local.delay_us = 0;
    local.period_us = 0;
    local.trigger_source = "button.top";
    for (const GoalFields &fields : {GoalFields{}, local}) {
        expect_whole_body_only(goal_body(fields), [](const uint8_t *body, uint32_t size) {
            GoalRecord record{};
            return ayni::work::read_goal_record(body, size, record);
        });
    }
}

// A goal type id is a catalog id: 1 to 32 bytes of the catalog alphabet.
TEST(WorkRecords, goal_record_needs_a_valid_goal_type_id) {
    for (const std::string &id : {std::string(), std::string(too_long_id), std::string("Reminder"),
                                  std::string("remind er")}) {
        GoalFields fields;
        fields.goal_type_id = id;
        EXPECT_FALSE(reads_goal(goal_body(fields))) << id;
    }
    GoalFields fields;
    fields.goal_type_id = long_id;
    EXPECT_TRUE(reads_goal(goal_body(fields)));

    // A length that runs past the body.
    Body cut;
    cut.u64(goal_id).u64(parent_goal_id).u64(owner).u64(replaced_goal_id).u64(principal);
    cut.u32(AYNI_ORIGIN_RULE).u32(AYNI_CHANNEL_MANAGEMENT).u32(3).u32(1).u32(2);
    cut.u32(200).raw("reminder");
    EXPECT_FALSE(reads_goal(cut));
}

// ADR-0021: a LOCAL_INPUT trigger names the local input it takes, a catalog
// id; any other trigger names none.
TEST(WorkRecords, goal_record_names_a_trigger_source_for_local_input_only) {
    const struct {
        ayni_trigger_kind_t trigger;
        std::string source;
        bool read;
    } cases[] = {
        {AYNI_TRIGGER_LOCAL_INPUT, "button.top", true},
        {AYNI_TRIGGER_LOCAL_INPUT, long_id, true},
        {AYNI_TRIGGER_LOCAL_INPUT, "", false},
        {AYNI_TRIGGER_LOCAL_INPUT, "Button", false},
        {AYNI_TRIGGER_LOCAL_INPUT, too_long_id, false},
        {AYNI_TRIGGER_NONE, "", true},
        {AYNI_TRIGGER_NONE, "button.top", false},
        {AYNI_TRIGGER_ONCE, "button.top", false},
        {AYNI_TRIGGER_DELAY, "button.top", false},
        {AYNI_TRIGGER_INTERVAL, "button.top", false},
        {AYNI_TRIGGER_INTERVAL, "Button", false},
    };
    for (const auto &item : cases) {
        GoalFields fields;
        fields.trigger = item.trigger;
        fields.delay_us = 0;
        fields.period_us = item.trigger == AYNI_TRIGGER_INTERVAL ? 1'000'000U : 0U;
        fields.trigger_source = item.source;
        EXPECT_EQ(reads_goal(goal_body(fields)), item.read) << item.trigger << "/" << item.source;
    }
}

// The parameters are the body's last field and end it exactly.
TEST(WorkRecords, goal_record_parameters_end_the_body) {
    GoalFields fields;
    fields.parameters.clear();
    const Body empty = goal_body(fields);
    EXPECT_TRUE(reads_goal(empty));

    constexpr uint32_t tail_size = 24;
    const std::vector<uint8_t> head(empty.bytes().begin(), empty.bytes().end() - tail_size - 4);
    const auto body = [&](uint32_t size) {
        return Body().raw(head).u32(size).raw("ab").u32(0).u64(0).u32(0).u32(0).u32(0);
    };
    EXPECT_FALSE(reads_goal(body(3)));
    EXPECT_FALSE(reads_goal(body(1)));
    EXPECT_TRUE(reads_goal(body(2)));
}

// GOAL_STATE: what a change left; whether a next occurrence and an expiry
// are pending, never when (C-STORE-014).
TEST(WorkRecords, goal_state_record_round_trips_each_flag) {
    for (const bool scheduled : {false, true}) {
        for (const bool expiring : {false, true}) {
            Goal goal = goal_of(GoalFields{});
            goal.last_task_id = task_id;
            goal.lifecycle = AYNI_GOAL_PAUSED;
            goal.revision = 8;
            goal.cancel_revision = 0;
            goal.occurrences_skipped = 3;
            goal.skip_reason = AYNI_REASON_TASK_CAPACITY;
            goal.cursor = {6, scheduled ? 500U : no_time, 2, 450};
            goal.expires_us = expiring ? 800U : no_time;
            const std::vector<uint8_t> body = written_goal_state(goal);
            EXPECT_EQ(load_le32(body.data() + 44),
                      (scheduled ? 0x1U : 0U) | (expiring ? 0x2U : 0U));

            GoalStateRecord record{};
            ASSERT_TRUE(ayni::work::read_goal_state_record(
                body.data(), ayni::work::goal_state_record_size, record));
            EXPECT_EQ(record.goal_id, goal_id);
            EXPECT_EQ(record.state.last_task_id, task_id);
            EXPECT_EQ(record.state.lifecycle, AYNI_GOAL_PAUSED);
            EXPECT_EQ(record.state.revision, 8U);
            EXPECT_EQ(record.state.cancel_revision, 0U);
            EXPECT_EQ(record.state.occurrences_skipped, 3U);
            EXPECT_EQ(record.state.skip_reason, AYNI_REASON_TASK_CAPACITY);
            EXPECT_EQ(record.state.taken, 6U);
            EXPECT_EQ(record.state.waiting, 2U);
            EXPECT_EQ(record.state.next_scheduled, scheduled);
            EXPECT_EQ(record.state.expiry_pending, expiring);
        }
    }
}

TEST(WorkRecords, goal_state_record_of_another_size_or_with_unknown_flags_is_refused) {
    expect_size_only(ayni::work::goal_state_record_size, [](const uint8_t *body, uint32_t size) {
        GoalStateRecord record{};
        return ayni::work::read_goal_state_record(body, size, record);
    });
    for (const uint32_t flags : {0x4U, 0x7U, 0x80000000U, 0xFFFFFFFFU}) {
        Body body;
        body.u64(goal_id).u64(task_id).u32(AYNI_GOAL_ACTIVE).u32(2).u32(0).u32(0).u32(0);
        body.u32(1).u32(0).u32(flags);
        GoalStateRecord record{};
        EXPECT_FALSE(ayni::work::read_goal_state_record(body.data(), body.size(), record)) << flags;
    }
}

// C-STORE-014: a record holds no time on this boot's clock. Goals that
// differ only in when they were created, and so in when their occurrences
// and expiry come, write the same GOAL and GOAL_STATE bodies; tasks that
// differ in when they fell due write the same TASK body.
TEST(WorkRecords, records_hold_no_time_of_the_boot_that_wrote_them) {
    const GoalFields fields;
    const Goal early = goal_of(fields, 100);
    const Goal late = goal_of(fields, 9'000'000);
    ASSERT_NE(early.cursor.next_due_us, late.cursor.next_due_us);
    ASSERT_NE(early.expires_us, late.expires_us);
    EXPECT_EQ(written_goal(early, fields.parameters), written_goal(late, fields.parameters));
    EXPECT_EQ(written_goal_state(early), written_goal_state(late));

    Task first = task_of();
    Task second = task_of();
    second.due_us = first.due_us + 5'000'000U;
    EXPECT_EQ(written_task(first), written_task(second));
}

// TASK: what the task was created with; its goal's place and its due time
// are this boot's.
TEST(WorkRecords, task_record_round_trips) {
    const std::vector<uint8_t> body = written_task(task_of());
    TaskRecord record{};
    ASSERT_TRUE(ayni::work::read_task_record(body.data(), ayni::work::task_record_size, record));
    EXPECT_EQ(record.task_id, task_id);
    EXPECT_EQ(record.replaced_task_id, 0x1'0000'0008ULL);
    EXPECT_EQ(record.task.goal_id, goal_id);
    EXPECT_EQ(record.task.owner_instance_id, owner);
    EXPECT_EQ(record.task.occurrence, 9U);
    EXPECT_EQ(record.task.occurrence_count, 4U);
    EXPECT_EQ(record.task.goal_index, 0U);
    EXPECT_EQ(record.task.due_us, 0U);
}

TEST(WorkRecords, task_record_of_another_size_is_refused) {
    expect_size_only(ayni::work::task_record_size, [](const uint8_t *body, uint32_t size) {
        TaskRecord record{};
        return ayni::work::read_task_record(body, size, record);
    });
}

// TASK_STATE: what a change left, a decision's hold included, whose status
// is a failure and so negative.
TEST(WorkRecords, task_state_record_round_trips_a_hold) {
    Task task = task_of();
    task.lifecycle = AYNI_TASK_CANCELLED;
    task.revision = 7;
    task.cancel_revision = 7;
    task.hold_status = AYNI_UNSUPPORTED_VERSION;
    task.hold_reason = AYNI_REASON_POLICY_NO_PROGRESS;
    task.step_count = 3;
    std::vector<uint8_t> body(ayni::work::task_state_record_size);
    ayni::work::write_task_state_record(task, body.data(), ayni::work::TaskRecordChange::state);

    TaskStateRecord record{};
    ASSERT_TRUE(ayni::work::read_task_state_record(body.data(), ayni::work::task_state_record_size,
                                                   record));
    EXPECT_EQ(record.task_id, task_id);
    EXPECT_EQ(record.state.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(record.state.revision, 7U);
    EXPECT_EQ(record.state.cancel_revision, 7U);
    EXPECT_EQ(record.state.hold_status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(record.state.hold_reason, AYNI_REASON_POLICY_NO_PROGRESS);
    EXPECT_EQ(record.state.step_count, 3U);
}

TEST(WorkRecords, task_state_record_of_another_size_is_refused) {
    expect_size_only(ayni::work::task_state_record_size, [](const uint8_t *body, uint32_t size) {
        TaskStateRecord record{};
        return ayni::work::read_task_state_record(body, size, record);
    });
}

// ADR-0041 / C-VER-023: pin the pre-refactor snapshot TASK bytes, including
// full-width history, independently of both codec directions.
TEST(WorkRecords, task_snapshot_matches_the_existing_byte_layout) {
    Task task{};
    task.task_id = 1;
    task.goal_id = 2;
    task.owner_instance_id = 3;
    task.replaced_task_id = 4;
    task.occurrence = 5;
    task.occurrence_count = 6;
    task.created_transition_id = 0x1817161514131211ULL;
    task.commit_seq = 0x2827262524232221ULL;
    task.steps_seq = 0x3837363534333231ULL;
    task.checkpoint_seq = 0x4847464544434241ULL;
    task.steps_before = 0x54535251U;
    std::vector<uint8_t> expected{1,    0,    0,    0,    0,    0,    0,    0,    2,    0,    0,
                                  0,    0,    0,    0,    0,    3,    0,    0,    0,    0,    0,
                                  0,    0,    4,    0,    0,    0,    0,    0,    0,    0,    5,
                                  0,    0,    0,    6,    0,    0,    0,    0x11, 0x12, 0x13, 0x14,
                                  0x15, 0x16, 0x17, 0x18, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                  0x28, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x41, 0x42,
                                  0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x51, 0x52, 0x53, 0x54};
    expected.insert(expected.begin() + 40, 24, 0);
    ASSERT_EQ(expected.size(), ayni::work::task_snapshot_record_size);
    std::vector<uint8_t> body(expected.size());
    ayni::work::write_task_snapshot_record(task, body.data(), false);
    EXPECT_EQ(body, expected);
    ayni::work::TaskSnapshotRecord record{};
    ASSERT_TRUE(ayni::work::read_task_snapshot_record(
        expected.data(), static_cast<uint32_t>(expected.size()), record));
    EXPECT_EQ(record.creation.task_id, task.task_id);
    EXPECT_EQ(record.creation.task.goal_id, task.goal_id);
    EXPECT_EQ(record.creation.task.owner_instance_id, task.owner_instance_id);
    EXPECT_EQ(record.creation.replaced_task_id, task.replaced_task_id);
    EXPECT_EQ(record.creation.task.occurrence, task.occurrence);
    EXPECT_EQ(record.creation.task.occurrence_count, task.occurrence_count);
    EXPECT_EQ(record.stamp.transition_id, task.created_transition_id);
    EXPECT_EQ(record.stamp.commit_seq, task.commit_seq);
    EXPECT_EQ(record.steps_seq, task.steps_seq);
    EXPECT_EQ(record.checkpoint_seq, task.checkpoint_seq);
    EXPECT_EQ(record.steps_before, task.steps_before);
    expect_size_only(ayni::work::task_snapshot_record_size,
                     [](const uint8_t *bytes, uint32_t size) {
                         ayni::work::TaskSnapshotRecord found{};
                         return ayni::work::read_task_snapshot_record(bytes, size, found);
                     });
}

// ADR-0041: a snapshot key can precede the most recent journal delta. The
// explicit range, rather than task.steps_before, determines its position.
TEST(WorkRecords, task_steps_snapshot_slice_keeps_an_earlier_key_and_its_position) {
    Tasks tasks(4);
    TaskTable &table = tasks.table();
    ASSERT_TRUE(table.insert({goal_id, owner, 0, 0, 1, 1}, task_id, {11, 1}));
    ASSERT_TRUE(table.add_step(0, {"a", 1}, {12, 2}));
    ASSERT_TRUE(table.add_step(0, {"bc", 2}, {12, 2}));
    ASSERT_TRUE(table.add_step(0, {"later", 5}, {13, 3}));
    const ayni::work::TaskStepRange slice{1, 1};
    const std::vector<uint8_t> expected{9, 0, 0, 0, 1, 0, 0, 0, 1, 0,   0,
                                        0, 1, 0, 0, 0, 2, 0, 0, 0, 'b', 'c'};
    const auto size = ayni::work::task_steps_record_size(table, 0, slice);
    ASSERT_EQ(size, expected.size());
    std::vector<uint8_t> body(size);
    ayni::work::write_task_steps_record(table, 0, slice, body.data());
    EXPECT_EQ(body, expected);
    uint64_t read_id = 0;
    Steps steps;
    ASSERT_TRUE(read_steps(expected.data(), size, read_id, steps));
    EXPECT_EQ(read_id, task_id);
    const Steps selected{{1, "bc"}};
    EXPECT_EQ(steps, selected);
}

// TASK_STEPS (ADR-0024): the steps the transition added, from the position
// of the first, each key after its length.
TEST(WorkRecords, task_steps_record_holds_the_steps_its_transition_added) {
    Tasks tasks(4);
    TaskTable &table = tasks.table();
    ASSERT_TRUE(table.insert({goal_id, owner, 0, 0, 1, 1}, task_id, {11, 1}));
    const auto key = [](const char *text) {
        return ayni_string_v1{text, static_cast<uint32_t>(std::strlen(text))};
    };
    ASSERT_TRUE(table.add_step(0, key("turn"), {12, 2}));
    ASSERT_TRUE(table.add_step(0, key("photo"), {12, 2}));
    const struct {
        Steps steps;
        uint32_t first;
    } expected[] = {{{{0, "turn"}, {1, "photo"}}, 0}, {{{2, long_id}, {3, "x"}}, 2}};
    for (int round = 0; round < 2; ++round) {
        if (round == 1) {
            ASSERT_TRUE(table.add_step(0, key(long_id), {13, 3}));
            ASSERT_TRUE(table.add_step(0, key("x"), {13, 3}));
        }
        uint32_t size = ayni::work::task_steps_record_fixed_size;
        for (const auto &step : expected[round].steps) {
            size += 4U + static_cast<uint32_t>(step.second.size());
        }
        const Task &task = table.at(0);
        const ayni::work::TaskStepRange range{task.steps_before,
                                              task.step_count - task.steps_before};
        ASSERT_EQ(ayni::work::task_steps_record_size(table, 0, range), size);
        std::vector<uint8_t> body = buffer_for(size);
        ayni::work::write_task_steps_record(table, 0, range, body.data());
        EXPECT_EQ(load_le32(body.data() + 8), expected[round].first);
        EXPECT_EQ(load_le32(body.data() + 12), 2U);

        uint64_t read_id = 0;
        Steps steps;
        ASSERT_TRUE(read_steps(body.data(), size, read_id, steps));
        EXPECT_EQ(read_id, task_id);
        EXPECT_EQ(steps, expected[round].steps);
        EXPECT_FALSE(read_steps(body.data(), size + 1U, read_id, steps));
    }
}

TEST(WorkRecords, task_steps_record_cut_short_or_followed_by_more_is_refused) {
    Body body;
    body.u64(task_id).u32(2).u32(2).text("turn").text("photo");
    expect_whole_body_only(body, [](const uint8_t *bytes, uint32_t size) {
        uint64_t read_id = 0;
        Steps steps;
        return read_steps(bytes, size, read_id, steps);
    });
}

// A transition that writes TASK_STEPS added at least one step, each key
// fits a catalog id, and the positions have numbers.
TEST(WorkRecords, task_steps_record_refuses_no_steps_and_impossible_ones) {
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(0)));
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(UINT32_MAX).u32(1).text("a")));
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(UINT32_MAX - 1U).u32(2).text("a").text("b")));
    EXPECT_TRUE(reads_steps(Body().u64(task_id).u32(UINT32_MAX - 1U).u32(1).text("a")));
    EXPECT_TRUE(reads_steps(Body().u64(task_id).u32(UINT32_MAX - 2U).u32(2).text("a").text("b")));
    // A key longer than a catalog id, and a length past the body.
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(1).text(too_long_id)));
    EXPECT_TRUE(reads_steps(Body().u64(task_id).u32(0).u32(1).text(long_id)));
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(1).u32(5).raw("abc")));
    // More steps than the body holds, however many.
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(2).text("a")));
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(UINT32_MAX).text("a")));
    // Bytes after the last key.
    EXPECT_FALSE(reads_steps(Body().u64(task_id).u32(0).u32(1).text("a").raw("b")));
}

// Replay stops at the first key its table refuses.
TEST(WorkRecords, task_steps_record_stops_where_its_reader_does) {
    Body body;
    body.u64(task_id).u32(4).u32(3).text("a").text("b").text("c");
    uint64_t read_id = 0;
    std::vector<uint32_t> seen;
    EXPECT_FALSE(ayni::work::read_task_steps_record(body.data(), body.size(), read_id,
                                                    [&seen](uint32_t step, ayni_string_v1) {
                                                        seen.push_back(step);
                                                        return step < 5U;
                                                    }));
    EXPECT_EQ(read_id, task_id);
    EXPECT_EQ(seen, (std::vector<uint32_t>{4, 5}));
}

// TASK_CHECKPOINT (ADR-0024, C-POLICY-010): the private checkpoint and the
// strategy and version that wrote it.
TEST(WorkRecords, task_checkpoint_record_round_trips_writer_and_state) {
    for (const std::string &state : {std::string("\x01\x02\x00\x04", 4), std::string()}) {
        Task task = task_of();
        task.checkpoint_writer_id = catalog_id(long_id);
        task.checkpoint_writer_version = 3;
        task.checkpoint_size = static_cast<uint32_t>(state.size());
        const uint32_t size = ayni::work::task_checkpoint_record_size(task);
        ASSERT_EQ(size, ayni::work::task_checkpoint_record_fixed_size + 32U + task.checkpoint_size);
        std::vector<uint8_t> body = buffer_for(size);
        ayni::work::write_task_checkpoint_record(task, bytes_of(state), body.data(), false);
        EXPECT_EQ(std::vector<uint8_t>(body.begin(), body.begin() + size),
                  Body().u64(task_id).u32(3).text(long_id).text(state).u32(0).bytes());

        TaskCheckpointRecord record{};
        ASSERT_TRUE(ayni::work::read_task_checkpoint_record(body.data(), size, record));
        EXPECT_EQ(record.task_id, task_id);
        EXPECT_EQ(record.writer_version, 3U);
        EXPECT_EQ(string_of(record.writer_id), long_id);
        EXPECT_EQ(string_of(record.state), state);
    }
}

// The writer's id fits a catalog id and the state ends the body; whether the
// id is a valid one is for restore_checkpoint() to say.
TEST(WorkRecords, task_checkpoint_record_refuses_a_long_writer_id_or_a_cut_state) {
    expect_whole_body_only(Body().u64(task_id).u32(3).text("plan").text("abcd").u32(0),
                           [](const uint8_t *body, uint32_t size) {
                               TaskCheckpointRecord record{};
                               return ayni::work::read_task_checkpoint_record(body, size, record);
                           });
    EXPECT_FALSE(reads_checkpoint(Body().u64(task_id).u32(3).text(too_long_id).text("ab")));
    EXPECT_FALSE(reads_checkpoint(Body().u64(task_id).u32(3).u32(60).raw("plan")));
    EXPECT_FALSE(reads_checkpoint(Body().u64(task_id).u32(3).text("plan").u32(3).raw("ab")));
    EXPECT_FALSE(reads_checkpoint(Body().u64(task_id).u32(3).text("plan").u32(1).raw("ab")));
    EXPECT_TRUE(reads_checkpoint(Body().u64(task_id).u32(3).text("plan").u32(2).raw("ab").u32(0)));
}

// COMMAND_MAPPING (ADR-0026 decision 9): the command accepted, its result
// and the principal's floor after it; the commit_seq is the batch's.
TEST(WorkRecords, command_record_round_trips_a_mapping_and_its_floor) {
    const CommandMapping mapping{
        principal, 0xFFFF'FFFF'FFFF'FFF0ULL, 0x0123'4567'89AB'CDEFULL,
        99,        AYNI_INTENT_GOAL_CANCEL,  {0, goal_id, 4, AYNI_OBJECT_GOAL}};
    std::vector<uint8_t> body = buffer_for(ayni::kernel::command_record_size);
    ayni::kernel::write_command_record(mapping, 0xFFFF'FFFF'FFFF'FF00ULL, body.data());
    EXPECT_EQ(std::vector<uint8_t>(body.begin(), body.end() - 1),
              Body()
                  .u64(principal)
                  .u64(0xFFFF'FFFF'FFFF'FFF0ULL)
                  .u64(0x0123'4567'89AB'CDEFULL)
                  .u64(0xFFFF'FFFF'FFFF'FF00ULL)
                  .u32(AYNI_INTENT_GOAL_CANCEL)
                  .u32(AYNI_OBJECT_GOAL)
                  .u64(goal_id)
                  .u32(4)
                  .u32(1)
                  .u32(AYNI_INTENT_EFFECT_CHANGED)
                  .u32(0)
                  .u32(0)
                  .u32(0)
                  .u32(0)
                  .bytes());

    CommandMapping read{};
    uint64_t floor = 0;
    ASSERT_TRUE(ayni::kernel::read_command_record(body.data(), ayni::kernel::command_record_size,
                                                  read, floor));
    EXPECT_EQ(read.principal_id, principal);
    EXPECT_EQ(read.command_id, 0xFFFF'FFFF'FFFF'FFF0ULL);
    EXPECT_EQ(read.digest, 0x0123'4567'89AB'CDEFULL);
    EXPECT_EQ(read.commit_seq, 0U);
    EXPECT_EQ(read.kind, AYNI_INTENT_GOAL_CANCEL);
    EXPECT_EQ(read.object.origin_device_id, 0U);
    EXPECT_EQ(read.object.id, goal_id);
    EXPECT_EQ(read.object.revision, 4U);
    EXPECT_EQ(read.object.kind, AYNI_OBJECT_GOAL);
    EXPECT_EQ(floor, 0xFFFF'FFFF'FFFF'FF00ULL);
}

// C-STORE-009 / ADR-0082: length checks start from a legal ordinary CHANGED receipt.
TEST(WorkRecords, command_record_of_another_size_is_refused) {
    const Body valid = Body()
                           .u64(1)
                           .u64(2)
                           .u64(3)
                           .u64(0)
                           .u32(AYNI_INTENT_GOAL_CANCEL)
                           .u32(AYNI_OBJECT_GOAL)
                           .u64(4)
                           .u32(1)
                           .u32(1)
                           .u32(AYNI_INTENT_EFFECT_CHANGED)
                           .u32(0)
                           .u32(0)
                           .u32(0)
                           .u32(0);
    expect_whole_body_only(valid, [](const uint8_t *body, uint32_t size) {
        CommandMapping mapping{};
        uint64_t floor = 0;
        return ayni::kernel::read_command_record(body, size, mapping, floor);
    });
}

} // namespace
