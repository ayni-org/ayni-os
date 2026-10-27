// ADR-0066: normal application behavior through public C API and runtime composition.
#include <ayni/applications/assistant.h>
#include <ayni/host_authority.h>
#include <ayni/runtime/common/demo_profile.h>
#include <ayni/runtime/sim_node.h>
#include <condition_variable>
#include <cstring>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace {
using namespace ayni::applications::assistant;
using namespace ayni::runtime;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
class AssistantApplication : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(node.assemble(demo_config()), AYNI_OK);
        node.set_utc(86400);
        const ayni::providers::presentation::Output output{
            this, [](void *ctx, ayni_string_v1 s) {
                auto &self = *static_cast<AssistantApplication *>(ctx);
                std::unique_lock<std::mutex> lock(self.output_mutex);
                if (!self.channel_available) {
                    return AYNI_UNSUPPORTED;
                }
                self.writer_entered = true;
                self.output_changed.notify_all();
                self.output_changed.wait(lock,
                                         [&] { return !self.block_output || self.release_output; });
                self.presented_output.emplace_back(s.data, s.size);
                return AYNI_OK;
            }};
        ASSERT_EQ(profile.assemble(node.kernel(), {}, {node.clock(), output, quiet}), AYNI_OK);
        ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
        pump();
        const ayni_session_desc_v1 session_desc{sizeof(session_desc), AYNI_CHANNEL_MANAGEMENT,
                                                demo_owner, demo_binding_revision, 0};
        ASSERT_EQ(ayni_open_session(node.kernel(), &session_desc, &session), AYNI_OK);
    }
    void TearDown() override {
        release_writer();
        ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
        pump();
        ayni_lifecycle_t lifecycle = 0;
        ASSERT_EQ(ayni_get_lifecycle(node.kernel(), &lifecycle), AYNI_OK);
        ASSERT_EQ(lifecycle, AYNI_LIFECYCLE_STOPPED);
        ASSERT_EQ(profile.quiesce(), AYNI_OK);
        ASSERT_EQ(node.release(), AYNI_OK);
        ASSERT_EQ(profile.close(), AYNI_OK);
    }
    void pump(uint32_t count = 80) {
        for (uint32_t i = 0; i < count; ++i) {
            ayni_lifecycle_t lifecycle = 0;
            ASSERT_EQ(ayni_get_lifecycle(node.kernel(), &lifecycle), AYNI_OK);
            if (lifecycle == AYNI_LIFECYCLE_STOPPED) {
                return;
            }
            node.advance(10000);
            ASSERT_EQ(profile.advance(), AYNI_OK);
            ayni_step_report_v1 report{};
            report.struct_size = sizeof(report);
            ASSERT_EQ(ayni_step(node.kernel(), 128, 100000, &report), AYNI_OK);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    uint64_t instance(uint64_t agent = demo_agent_b) {
        ayni_agent_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_agent(node.kernel(), agent, &info), AYNI_OK);
        return info.instance_id;
    }
    ayni_intent_result_v1 submit(ayni_intent_kind_t kind, const void *body, uint32_t revision = 0) {
        const ayni_intent_v1 intent{sizeof(intent), kind, ++command, session, revision, 0, body};
        ayni_intent_result_v1 result{};
        result.struct_size = sizeof(result);
        ayni_error_v1 error{};
        error.struct_size = sizeof(error);
        EXPECT_EQ(ayni_submit_intent(node.kernel(), &intent, &result, &error), AYNI_OK)
            << error.reason;
        pump();
        return result;
    }
    void allow(const char *alias, uint64_t agent = demo_agent_b) {
        const ayni_grant_create_v1 body{sizeof(body),
                                        1,
                                        profile.capability(alias),
                                        instance(agent),
                                        0,
                                        AYNI_GRANT_PERMANENT,
                                        0,
                                        0,
                                        profile.scope(alias)};
        submit(AYNI_INTENT_GRANT_CREATE, &body);
    }
    uint64_t create(Parameters p, const ayni_calendar_rule_v1 *calendar = nullptr) {
        Encoded encoded;
        ayni_goal_create_v1 goal{};
        EXPECT_EQ(prepare_goal(p, instance(), check_period_us, calendar, encoded, goal), AYNI_OK);
        return submit(AYNI_INTENT_GOAL_CREATE, &goal).object.id;
    }
    ayni_goal_info_v1 goal(uint64_t id) {
        ayni_goal_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_goal(node.kernel(), id, &out), AYNI_OK);
        return out;
    }
    ayni_task_info_v1 task(uint64_t goal_id) {
        ayni_task_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_task(node.kernel(), goal(goal_id).last_task_id, &out), AYNI_OK);
        return out;
    }
    uint64_t save(const char *body) {
        const ayni_memory_save_v1 request{sizeof(request),
                                          AYNI_MEMORY_PRIVATE,
                                          0,
                                          instance(),
                                          AYNI_PROFILE_NONE,
                                          0,
                                          text(body),
                                          nullptr,
                                          0,
                                          0,
                                          nullptr};
        return submit(AYNI_INTENT_MEMORY_SAVE, &request).object.id;
    }
    DemoProfile profile;
    SimNode node;
    QuietHours quiet;
    uint64_t session = 0, command = 0;
    std::mutex output_mutex;
    std::condition_variable output_changed;
    bool block_output = false, release_output = false, writer_entered = false;
    bool channel_available = true;
    std::vector<std::string> presented_output;
    std::vector<std::string> presented() {
        std::lock_guard<std::mutex> lock(output_mutex);
        return presented_output;
    }
    void release_writer() {
        std::lock_guard<std::mutex> lock(output_mutex);
        release_output = true;
        output_changed.notify_all();
    }
};
class QuietAssistantApplication : public AssistantApplication {
  protected:
    QuietAssistantApplication() { quiet = {true, 22 * 60, 7 * 60, "UTC0"}; }
};
class UnavailableAssistantApplication : public AssistantApplication {
  protected:
    UnavailableAssistantApplication() { channel_available = false; }
};
TEST_F(UnavailableAssistantApplication,
       unsupported_channel_preserves_pending_reminder_and_projects_failure) {
    allow("notify");
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400;
    const auto id = create({GoalKind::reminder, "等待处理", {}}, &rule);
    EXPECT_EQ(goal(id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(task(id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(task(id).hold_status, AYNI_UNSUPPORTED);
    EXPECT_TRUE(presented().empty());
    Activity activity;
    ASSERT_EQ(project(node.kernel(), activity), AYNI_OK);
    EXPECT_EQ(activity.primary, State::needs_attention);
}
TEST_F(QuietAssistantApplication,
       due_notification_waits_without_blocking_the_legacy_channel_then_presents_once) {
    allow("notify");
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400;
    const auto id = create({GoalKind::reminder, "安静后呈现", {}}, &rule);
    EXPECT_TRUE(presented().empty());
    EXPECT_NE(goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    ayni_resource_info_v1 legacy{};
    legacy.struct_size = sizeof(legacy);
    ASSERT_EQ(ayni_get_resource(node.kernel(), text("presentation.slot"), &legacy), AYNI_OK);
    EXPECT_EQ(legacy.used, 0U);
    node.set_utc(86400 + 8 * 3600, AYNI_UTC_UNTRUSTED);
    pump();
    EXPECT_TRUE(presented().empty());
    node.set_utc(86400 + 8 * 3600);
    pump();
    ASSERT_EQ(presented().size(), 1U);
    EXPECT_EQ(goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    pump();
    EXPECT_EQ(presented().size(), 1U);
}
// ADR-0066: in-flight output can finish; queued output must check actual time,
// even while Core's last resource projection still says the window is online.
TEST_F(QuietAssistantApplication,
       blocked_writer_keeps_owner_responsive_and_queued_output_rechecks_time) {
    node.set_utc(86400 + 12 * 3600);
    pump();
    allow("notify");
    {
        std::lock_guard<std::mutex> lock(output_mutex);
        block_output = true;
    }
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400 + 12 * 3600;
    const auto first = create({GoalKind::reminder, "已开始", {}}, &rule);
    {
        std::unique_lock<std::mutex> lock(output_mutex);
        ASSERT_TRUE(
            output_changed.wait_for(lock, std::chrono::seconds(5), [&] { return writer_entered; }));
    }
    const ayni_pause_group_set_v1 pause{sizeof(pause), autonomous_group, 1, 0};
    submit(AYNI_INTENT_PAUSE_GROUP_SET, &pause);
    ayni_pause_group_info_v1 group{};
    group.struct_size = sizeof(group);
    ASSERT_EQ(ayni_get_pause_group(node.kernel(), autonomous_group, &group), AYNI_OK);
    EXPECT_EQ(group.paused, 1U);
    const auto second = create({GoalKind::reminder, "尚未开始", {}}, &rule);
    node.set_utc(86400 + 23 * 3600);
    release_writer();
    for (uint32_t i = 0; i < 100; ++i) {
        // Deliberately retain the previous owner resource projection; the worker
        // independently reads current UTC before the second operation starts.
        node.advance(10000);
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        ASSERT_EQ(ayni_step(node.kernel(), 128, 100000, &report), AYNI_OK);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    EXPECT_EQ(goal(first).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(presented().size(), 1U);
    EXPECT_EQ(goal(second).lifecycle, AYNI_GOAL_ACTIVE);
    ayni_resource_info_v1 channel{};
    channel.struct_size = sizeof(channel);
    ASSERT_EQ(ayni_get_resource(node.kernel(), text("notification.slot"), &channel), AYNI_OK);
    EXPECT_EQ(channel.used, 1U);
    node.set_utc(2 * 86400 + 8 * 3600);
    pump();
    EXPECT_EQ(goal(second).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(presented().size(), 2U);
}
TEST_F(AssistantApplication, pausing_autonomy_preserves_reminders_and_new_goal_membership) {
    allow("notify");
    allow("health");
    const ayni_pause_group_set_v1 pause{sizeof(pause), autonomous_group, 1, 0};
    submit(AYNI_INTENT_PAUSE_GROUP_SET, &pause);
    const auto check = create({GoalKind::device_check, {}, {}});
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400;
    const auto reminder = create({GoalKind::reminder, "喝水", {}}, &rule);
    EXPECT_EQ(goal(check).group_paused, 1U);
    EXPECT_EQ(goal(reminder).pause_group, 0U);
    EXPECT_EQ(goal(reminder).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(presented().size(), 1U);
    EXPECT_NE(presented().front().find("喝水"), std::string::npos);
    EXPECT_NE(presented().front().find("1970-01-02 00:00:00"), std::string::npos);
    pump();
    EXPECT_EQ(presented().size(), 1U);
}
TEST_F(AssistantApplication, maximum_reminder_text_leaves_space_for_the_original_civil_time) {
    allow("notify");
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400;
    const auto body = std::string(text_bytes_max, 'x');
    const auto id = create({GoalKind::reminder, body, {}}, &rule);
    EXPECT_EQ(goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(presented().size(), 1U);
    EXPECT_EQ(presented().front().find(body), 0U);
    EXPECT_LE(presented().front().size(), ayni::providers::presentation::text_bytes_max);
}
TEST_F(AssistantApplication,
       explicit_memory_sources_produce_unconfirmed_summary_and_duplicate_candidates) {
    allow("summary");
    const auto first = save("I like tea"), second = save("I   LIKE tea");
    const auto id = create({GoalKind::memory_organize, {}, {first, second}});
    pump();
    ASSERT_EQ(task(id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(task(id).model_calls_used, 1U);
    const ayni_memory_search_v1 query{sizeof(query), AYNI_MEMORY_PRIVATE, instance(), 0, {}, {}};
    ayni_memory_info_v1 records[8]{};
    uint32_t count = 0;
    ASSERT_EQ(ayni_search_memory(node.kernel(), session, &query, records, 8, &count), AYNI_OK);
    ASSERT_EQ(count, 4U);
    uint32_t candidates = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (records[i].memory_id == first || records[i].memory_id == second) {
            EXPECT_EQ(records[i].confirmed, 1U);
            EXPECT_EQ(records[i].state, AYNI_MEMORY_LIVE);
        } else {
            ++candidates;
            EXPECT_EQ(records[i].confirmed, 0U);
            EXPECT_GE(records[i].dependency_count, 2U);
            EXPECT_EQ(records[i].usable, 1U);
        }
    }
    EXPECT_EQ(candidates, 2U);
}
TEST_F(AssistantApplication,
       continuous_observer_waits_for_current_samples_and_stops_at_model_limit) {
    allow("summary");
    allow("observe");
    const auto id = create({GoalKind::scene_observe, {}, {}});
    EXPECT_EQ(task(id).lifecycle, AYNI_TASK_WAITING)
        << task(id).hold_status << ':' << task(id).hold_reason;
    EXPECT_EQ(task(id).model_calls_used, 1U);
    for (uint32_t i = 0; i < 200 && !task(id).model_call_limit_reached; ++i) {
        node.advance(1000000);
        node.set_utc(86400 + static_cast<int64_t>(i + 1));
        pump(20);
    }
    EXPECT_EQ(task(id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(task(id).model_calls_used, AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT);
    EXPECT_EQ(task(id).model_call_limit_reached, 1U);
}
// ADR-0066: pause cancels the accepted analysis round; resume uses a new generation/key.
TEST_F(AssistantApplication, paused_analysis_round_is_retained_and_new_generation_continues) {
    allow("summary");
    allow("observe");
    const auto id = create({GoalKind::scene_observe, {}, {}});
    node.set_utc(86400 + 300);
    node.advance(1000000);
    for (uint32_t i = 0; i < 200 && task(id).model_calls_used < 2; ++i) {
        pump(1);
    }
    ASSERT_EQ(task(id).model_calls_used, 2U);
    const auto original = task(id).logical_call_id;
    ASSERT_NE(original, 0U);
    const auto previous_generation = task(id).wait_generation;
    const ayni_pause_group_set_v1 pause{sizeof(pause), autonomous_group, 1, 0};
    submit(AYNI_INTENT_PAUSE_GROUP_SET, &pause);
    ayni_call_info_v1 cancelled{};
    cancelled.struct_size = sizeof(cancelled);
    ASSERT_EQ(ayni_get_call(node.kernel(), original, &cancelled), AYNI_OK);
    EXPECT_EQ(cancelled.outcome, AYNI_CALL_CANCELLED);
    const ayni_pause_group_set_v1 resume{sizeof(resume), autonomous_group, 0, 0};
    submit(AYNI_INTENT_PAUSE_GROUP_SET, &resume, 1);
    pump();
    EXPECT_EQ(task(id).lifecycle, AYNI_TASK_WAITING)
        << task(id).hold_status << ':' << task(id).hold_reason;
    EXPECT_EQ(task(id).model_calls_used, 3U);
    EXPECT_GT(task(id).wait_generation, previous_generation);
    ASSERT_EQ(ayni_get_call(node.kernel(), original, &cancelled), AYNI_OK);
    EXPECT_EQ(cancelled.outcome, AYNI_CALL_CANCELLED);
}
TEST_F(AssistantApplication, presented_reminder_is_not_replayed_after_orderly_close_and_reopen) {
    allow("notify");
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_ONCE;
    rule.rule_version = 1;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", rule.timezone_size);
    rule.first_local_s = 86400;
    const auto id = create({GoalKind::reminder, "已确认", {}}, &rule);
    ASSERT_EQ(goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(presented().size(), 1U);
    TearDown();
    SetUp();
    pump();
    EXPECT_EQ(goal(id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(presented().size(), 1U);
}
TEST(AssistantPolicy, routing_encoding_quiet_time_and_conflict_projection_are_explicit) {
    ayni_calendar_civil_v1 civil{};
    civil.struct_size = sizeof(civil);
    ASSERT_EQ(ayni_calendar_split_time(AYNI_CALENDAR_SECONDS_MAX, &civil), AYNI_OK);
    EXPECT_EQ(civil.year, 9999);
    EXPECT_EQ(civil.month, 12);
    EXPECT_EQ(civil.day, 31);
    EXPECT_EQ(civil.hour, 23);
    EXPECT_EQ(civil.minute, 59);
    EXPECT_EQ(civil.second, 59);
    std::string body;
    EXPECT_EQ(route("@b check", 1, 2, body), 2U);
    EXPECT_EQ(body, "check");
    EXPECT_EQ(route("检查", 1, 2, body), 1U);
    EXPECT_EQ(route("普通提问", 1, 2, body), 2U);
    Encoded encoded;
    Parameters original{GoalKind::memory_organize, {}, {1, 2}}, copy;
    ASSERT_EQ(encode(original, encoded), AYNI_OK);
    ASSERT_EQ(decode(encoded.view(), copy), AYNI_OK);
    EXPECT_EQ(copy.sources, original.sources);
    QuietHours quiet{true, 22 * 60, 7 * 60, "UTC0"};
    bool allowed = true;
    ASSERT_EQ(quiet.allows({sizeof(ayni_utc_sample_v1), AYNI_UTC_UNTRUSTED, 12 * 3600}, allowed),
              AYNI_OK);
    EXPECT_FALSE(allowed);
    ASSERT_EQ(quiet.allows({sizeof(ayni_utc_sample_v1), AYNI_UTC_TRUSTED, 23 * 3600}, allowed),
              AYNI_OK);
    EXPECT_FALSE(allowed);
    ASSERT_EQ(quiet.allows({sizeof(ayni_utc_sample_v1), AYNI_UTC_TRUSTED, 7 * 3600}, allowed),
              AYNI_OK);
    EXPECT_TRUE(allowed);
    const auto merged = merge_suggestions(
        {{"name", "A", 1, false}, {"name", "A", 2, false}, {"name", "B", 2, false}});
    ASSERT_EQ(merged.size(), 2U);
    EXPECT_EQ(merged[0].agents, 3U);
    EXPECT_TRUE(merged[0].conflict);
    EXPECT_TRUE(merged[1].conflict);
}
} // namespace
