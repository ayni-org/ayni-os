// ADR-0060: public management intents, durable memory facts and bounded private/profile queries.
#include "work_node.h"
#include <ayni/memory.h>
namespace {
using namespace ayni::test;
class KernelMemory : public testing::Test {
  protected:
    WorkNode work;
    void SetUp() override {
        work.max_checkpoint_bytes = 8;
        work.node().config().memory_capacity = 3;
        work.node().config().memory_body_max_bytes = 64;
        work.node().config().memory_dependency_capacity = 3;
        work.node().config().storage_buffer_bytes = 0;
        work.run();
    }
    Submitted save(uint64_t command, const char *body, uint64_t id = 0, uint32_t revision = 0,
                   uint64_t session = 0, ayni_memory_namespace_t space = AYNI_MEMORY_PRIVATE) {
        const ayni_string_v1 tag = text("偏好");
        const ayni_string_v1 keyword = text("饮料");
        const ayni_memory_save_v1 value{
            sizeof(value),
            space,
            id,
            space == AYNI_MEMORY_PRIVATE ? work.instance(agent_a) : owner_id,
            space == AYNI_MEMORY_PRIVATE ? AYNI_PROFILE_NONE : AYNI_PROFILE_LANGUAGE,
            0,
            text(body),
            &tag,
            1,
            1,
            &keyword};
        const ayni_intent_v1 intent{sizeof(intent), AYNI_INTENT_MEMORY_SAVE,
                                    command,        session == 0 ? work.session() : session,
                                    revision,       0,
                                    &value};
        return work.submit(intent);
    }
    Submitted change(ayni_intent_kind_t kind, uint64_t command, uint64_t id, uint32_t revision) {
        const ayni_memory_target_v1 body{sizeof(body), 0, id};
        return work.submit(
            {sizeof(ayni_intent_v1), kind, command, work.session(), revision, 0, &body});
    }
    void durable(uint64_t sequence) {
        for (unsigned round = 0; round < 100 && work.node().info().durable_seq < sequence;
             ++round) {
            work.node().sim().advance_us(10000);
            work.node().step();
            work.node().sim().complete_storage();
        }
        ASSERT_GE(work.node().info().durable_seq, sequence);
    }
    ayni_memory_info_v1 info(uint64_t id) {
        ayni_memory_info_v1 result{};
        result.struct_size = sizeof(result);
        uint32_t bytes = 99;
        EXPECT_EQ(ayni_get_memory(work.kernel(), work.session(), id, &result, nullptr, 0, &bytes),
                  AYNI_OK);
        EXPECT_EQ(bytes, 0U);
        return result;
    }
    std::string body(uint64_t id) {
        std::array<char, 64> buffer{};
        ayni_memory_info_v1 result{};
        result.struct_size = sizeof(result);
        uint32_t bytes = 0;
        EXPECT_EQ(ayni_get_memory(work.kernel(), work.session(), id, &result, buffer.data(),
                                  buffer.size(), &bytes),
                  AYNI_OK);
        return std::string(buffer.data(), bytes);
    }
};
TEST_F(KernelMemory, explicit_save_is_confirmed_and_body_waits_for_durable_receipt) {
    const auto saved = save(1, "偏好绿茶");
    ASSERT_EQ(saved.status, AYNI_OK);
    EXPECT_EQ(saved.result.object.kind, AYNI_OBJECT_MEMORY);
    const auto id = saved.result.object.id;
    EXPECT_EQ(info(id).confirmed, 1U);
    EXPECT_EQ(info(id).source, AYNI_MEMORY_USER);
    EXPECT_EQ(change(AYNI_INTENT_MEMORY_CONFIRM, 2, id, 1).status, AYNI_INVALID_STATE);
    EXPECT_EQ(info(id).usable, 0U);
    durable(saved.result.commit_seq);
    EXPECT_EQ(body(id), "偏好绿茶");
    EXPECT_EQ(info(id).usable, 1U);
}
TEST_F(KernelMemory, exact_retry_and_revision_cas_do_not_replace_other_content) {
    const auto first = save(1, "first");
    ASSERT_EQ(first.status, AYNI_OK);
    const auto retry = save(1, "first");
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, first.result.commit_seq);
    EXPECT_EQ(save(1, "different").status, AYNI_IDEMPOTENCY_CONFLICT);
    EXPECT_EQ(save(2, "second", first.result.object.id, 2).status, AYNI_REVISION_CONFLICT);
    const auto edited = save(2, "second", first.result.object.id, 1);
    ASSERT_EQ(edited.status, AYNI_OK);
    durable(edited.result.commit_seq);
    EXPECT_EQ(body(first.result.object.id), "second");
    EXPECT_EQ(info(first.result.object.id).revision, 2U);
}
TEST_F(KernelMemory, management_authentication_applies_to_queries_and_exact_retries) {
    const auto saved = save(1, "private");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    const auto voice = work.open_session(AYNI_CHANNEL_VOICE);
    EXPECT_EQ(save(1, "private", 0, 0, voice).status, AYNI_DENIED);
    ayni_memory_info_v1 result{};
    result.struct_size = sizeof(result);
    uint32_t bytes = 91;
    EXPECT_EQ(
        ayni_get_memory(work.kernel(), voice, saved.result.object.id, &result, nullptr, 0, &bytes),
        AYNI_DENIED);
    EXPECT_EQ(bytes, 91U);
}
TEST_F(KernelMemory, unicode_search_keeps_private_namespaces_and_profile_separate) {
    const auto private_saved = save(1, "绿茶");
    ASSERT_EQ(private_saved.status, AYNI_OK);
    const auto public_saved = save(2, "中文", 0, 0, 0, AYNI_MEMORY_PROFILE);
    ASSERT_EQ(public_saved.status, AYNI_OK);
    durable(public_saved.result.commit_seq);
    std::array<ayni_memory_info_v1, 3> result{};
    uint32_t count = 0;
    ayni_memory_search_v1 query{sizeof(query), AYNI_MEMORY_PRIVATE, work.instance(agent_a), 0,
                                text("饮料"),  text("偏好")};
    ASSERT_EQ(ayni_search_memory(work.kernel(), work.session(), &query, result.data(),
                                 result.size(), &count),
              AYNI_OK);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(result[0].memory_id, private_saved.result.object.id);
    query.namespace_id = work.instance(agent_b);
    ASSERT_EQ(ayni_search_memory(work.kernel(), work.session(), &query, result.data(),
                                 result.size(), &count),
              AYNI_OK);
    EXPECT_EQ(count, 0U);
    query.name_space = AYNI_MEMORY_PROFILE;
    query.namespace_id = owner_id;
    ASSERT_EQ(ayni_search_memory(work.kernel(), work.session(), &query, result.data(),
                                 result.size(), &count),
              AYNI_OK);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(result[0].profile_field, AYNI_PROFILE_LANGUAGE);
}
TEST_F(KernelMemory, deletion_blocks_body_before_storage_acknowledgement) {
    const auto saved = save(1, "erase me");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    const auto deleted = change(AYNI_INTENT_MEMORY_DELETE, 2, saved.result.object.id, 1);
    ASSERT_EQ(deleted.status, AYNI_OK);
    const auto state = info(saved.result.object.id);
    EXPECT_EQ(state.state, AYNI_MEMORY_TOMBSTONE);
    EXPECT_EQ(state.usable, 0U);
    std::array<char, 64> untouched{};
    auto out = state;
    uint32_t bytes = 123;
    EXPECT_EQ(ayni_get_memory(work.kernel(), work.session(), saved.result.object.id, &out,
                              untouched.data(), untouched.size(), &bytes),
              AYNI_EXPIRED);
    EXPECT_EQ(bytes, 123U);
}
TEST_F(KernelMemory, normal_reopen_preserves_body_namespace_and_original_receipt) {
    const auto saved = save(1, "saved preference");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(body(saved.result.object.id), "saved preference");
    const auto retry = save(1, "saved preference");
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, saved.result.commit_seq);
}
TEST_F(KernelMemory, full_capacity_and_small_output_buffers_leave_saved_facts_intact) {
    const auto saved = save(1, "retained");
    ASSERT_EQ(saved.status, AYNI_OK);
    ASSERT_EQ(save(2, "two").status, AYNI_OK);
    const auto last = save(3, "three");
    ASSERT_EQ(last.status, AYNI_OK);
    EXPECT_EQ(save(4, "four").status, AYNI_CAPACITY_EXCEEDED);
    durable(last.result.commit_seq);
    auto out = info(saved.result.object.id);
    char tiny = 'x';
    uint32_t bytes = 0;
    EXPECT_EQ(ayni_get_memory(work.kernel(), work.session(), saved.result.object.id, &out, &tiny, 1,
                              &bytes),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(tiny, 'x');
    EXPECT_EQ(bytes, 8U);
    EXPECT_EQ(body(saved.result.object.id), "retained");
}

// ADR-0060: accepted reads persist their complete dependencies with the checkpoint.
TEST_F(KernelMemory, strategy_read_checkpoint_survives_reopen_and_source_delete_blocks_export) {
    const auto saved = save(1, "tea");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        EXPECT_NE(snapshot.memory, nullptr);
        ayni_memory_info_v1 metadata{};
        metadata.struct_size = sizeof(metadata);
        uint32_t bytes = 0;
        const auto status = snapshot.memory->read(
            snapshot.memory->context, saved.result.object.id, AYNI_MEMORY_CONTEXT, &metadata,
            reinterpret_cast<char *>(out.checkpoint), out.checkpoint_capacity, &bytes);
        EXPECT_EQ(status, AYNI_OK);
        out.checkpoint_size = bytes;
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
    };
    const auto created = work.create(WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                                           {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    uint64_t task_id = 0;
    for (unsigned i = 0; i < 100; ++i) {
        work.node().sim().advance_us(10000);
        work.node().step();
        work.node().sim().complete_storage();
        task_id = work.goal(created.result.object.id).last_task_id;
        if (task_id != 0 && work.task(task_id).lifecycle == AYNI_TASK_COMPLETED) {
            break;
        }
    }
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
    durable(work.task(task_id).commit_seq);
    std::array<uint8_t, 8> checkpoint{};
    uint32_t size = 0;
    ASSERT_EQ(ayni_copy_task_checkpoint(work.kernel(), task_id, checkpoint.data(),
                                        checkpoint.size(), &size),
              AYNI_OK);
    EXPECT_EQ(std::string(checkpoint.begin(), checkpoint.begin() + size), "tea");
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task_id, checkpoint.data(),
                                        checkpoint.size(), &size),
              AYNI_OK);
    ASSERT_EQ(change(AYNI_INTENT_MEMORY_DELETE, 2, saved.result.object.id, 1).status, AYNI_OK);
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task_id, checkpoint.data(),
                                        checkpoint.size(), &size),
              AYNI_EXPIRED);
}
// Ignoring a denied read in a native policy cannot publish a successful contaminated decision.
TEST_F(KernelMemory, another_agent_cannot_read_private_content_or_ignore_the_denial) {
    const auto saved = save(1, "tea");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        std::array<char, 64> body{};
        ayni_memory_info_v1 metadata{};
        metadata.struct_size = sizeof(metadata);
        uint32_t bytes = 97;
        EXPECT_EQ(snapshot.memory->read(snapshot.memory->context, saved.result.object.id,
                                        AYNI_MEMORY_CONTEXT, &metadata, body.data(), body.size(),
                                        &bytes),
                  AYNI_DENIED);
        EXPECT_EQ(bytes, 97U);
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
    };
    const auto created = work.create(WorkNode::create_body(work.instance(agent_b), 0, "reminder", 1,
                                                           {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    uint64_t task_id = 0;
    for (unsigned i = 0; i < 100 && work.strategy().seen.empty(); ++i) {
        work.node().step();
        work.node().sim().complete_storage();
        task_id = work.goal(created.result.object.id).last_task_id;
    }
    ASSERT_FALSE(work.strategy().seen.empty());
    ASSERT_NE(task_id, 0U);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task_id).hold_reason, AYNI_REASON_MEMORY_SOURCE);
}

// ADR-0060: policy writes are unconfirmed, keep inherited sources through owner confirmation,
// and commit with a unique step/checkpoint so ordinary reopening cannot repeat the write.
TEST_F(KernelMemory, policy_derived_memory_and_confirmation_retain_sources_across_reopen) {
    const auto saved = save(1, "tea");
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        if (snapshot.step_count != 0) {
            return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
        }
        std::array<char, 64> source{};
        ayni_memory_info_v1 metadata{};
        metadata.struct_size = sizeof(metadata);
        uint32_t bytes = 0;
        EXPECT_EQ(snapshot.memory->read(snapshot.memory->context, saved.result.object.id,
                                        AYNI_MEMORY_ORGANIZE, &metadata, source.data(),
                                        source.size(), &bytes),
                  AYNI_OK);
        ayni_memory_proposal_v1 proposal{};
        proposal.struct_size = sizeof(proposal);
        proposal.source = AYNI_MEMORY_DERIVED;
        proposal.step_key = text("organize");
        proposal.body = text("green tea");
        EXPECT_EQ(snapshot.memory->propose(snapshot.memory->context, &proposal), AYNI_OK);
        out.intents[0] = policy_intent(AYNI_POLICY_WRITE_MEMORY, snapshot.revision, "organize");
        out.intent_count = 1;
        return AYNI_OK;
    };
    const auto created = work.create(WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                                           {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    uint64_t task_id = 0;
    for (unsigned i = 0; i < 100; ++i) {
        work.node().sim().advance_us(10000);
        work.node().step();
        work.node().sim().complete_storage();
        task_id = work.goal(created.result.object.id).last_task_id;
        if (task_id != 0 && work.task(task_id).lifecycle == AYNI_TASK_COMPLETED) {
            break;
        }
    }
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
    durable(work.task(task_id).commit_seq);
    ayni_memory_search_v1 query{sizeof(query), AYNI_MEMORY_PRIVATE, work.instance(agent_a), 0,
                                text("green"), {nullptr, 0}};
    std::array<ayni_memory_info_v1, 3> rows{};
    uint32_t count = 0;
    ASSERT_EQ(
        ayni_search_memory(work.kernel(), work.session(), &query, rows.data(), rows.size(), &count),
        AYNI_OK);
    ASSERT_EQ(count, 1U);
    const auto derived = rows[0].memory_id;
    EXPECT_EQ(rows[0].source, AYNI_MEMORY_DERIVED);
    EXPECT_EQ(rows[0].confirmed, 0U);
    EXPECT_EQ(rows[0].dependency_count, 1U);
    const auto confirmed = change(AYNI_INTENT_MEMORY_CONFIRM, 2, derived, 1);
    ASSERT_EQ(confirmed.status, AYNI_OK);
    durable(confirmed.result.commit_seq);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(body(derived), "green tea");
    EXPECT_EQ(info(derived).confirmed, 1U);
    EXPECT_EQ(info(derived).dependency_count, 1U);
    ASSERT_EQ(change(AYNI_INTENT_MEMORY_DELETE, 3, saved.result.object.id, 1).status, AYNI_OK);
    EXPECT_EQ(info(derived).usable, 0U);
}

// ADR-0060: a nonmatch still influences a search result, so it remains a captured source.
TEST_F(KernelMemory, strategy_search_tracks_authorized_nonmatches_before_publishing_a_page) {
    const auto first = save(1, "coffee");
    const auto second = save(2, "tea");
    ASSERT_EQ(first.status, AYNI_OK);
    ASSERT_EQ(second.status, AYNI_OK);
    durable(second.result.commit_seq);
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        const ayni_memory_search_v1 query{
            sizeof(query), AYNI_MEMORY_PRIVATE, snapshot.owner_instance_id, 0,
            text("tea"),   {nullptr, 0}};
        std::array<ayni_memory_info_v1, 3> rows{};
        uint32_t count = 0;
        EXPECT_EQ(snapshot.memory->search(snapshot.memory->context, &query, AYNI_MEMORY_CONTEXT,
                                          rows.data(), rows.size(), &count),
                  AYNI_OK);
        EXPECT_EQ(count, 1U);
        EXPECT_EQ(rows[0].memory_id, second.result.object.id);
        std::memcpy(out.checkpoint, "tea", 3);
        out.checkpoint_size = 3;
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
    };
    const auto created = work.create(WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                                           {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    uint64_t task_id = 0;
    for (unsigned i = 0; i < 100; ++i) {
        work.node().sim().advance_us(10000);
        work.node().step();
        work.node().sim().complete_storage();
        task_id = work.goal(created.result.object.id).last_task_id;
        if (task_id != 0 && work.task(task_id).lifecycle == AYNI_TASK_COMPLETED) {
            break;
        }
    }
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
    durable(work.task(task_id).commit_seq);
    ASSERT_EQ(change(AYNI_INTENT_MEMORY_DELETE, 3, first.result.object.id, 1).status, AYNI_OK);
    std::array<uint8_t, 8> checkpoint{};
    uint32_t size = 0;
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task_id, checkpoint.data(),
                                        checkpoint.size(), &size),
              AYNI_EXPIRED);
}
} // namespace
