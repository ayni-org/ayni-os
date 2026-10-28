// ADR-0060: context publication proves the original Task basis, while deletion invalidates
// every dependent consumer before external files are acknowledged.
#include "goal_approval_request.h"
#include "journal_test_support.h"
#include "kernel/service/memory/privacy_service.h"
#include "knowledge/context/context_records.h"
#include "snapshot_test_support.h"
#include "work/provenance/provenance_table.h"
#include "work_node.h"
#include <ayni/context.h>
#include <ayni/memory.h>
namespace {
using namespace ayni::test;
class KernelContext : public testing::Test {
  protected:
    Node node;
    ScriptedStrategy strategy;
    uint64_t session = 0;
    uint64_t owner = 0;
    uint64_t next_command = 1;
    bool require_approval = false;
    uint32_t registered_version = 1;
    uint32_t registered_context_mode = AYNI_GOAL_CONTEXT_AGENT;
    void SetUp() override {
        node.config().goal_capacity = 2;
        node.config().command_capacity = 32;
        node.config().goal_parameter_max_bytes = 128;
        node.config().memory_capacity = 4;
        node.config().memory_body_max_bytes = 64;
        node.config().memory_dependency_capacity = 4;
        node.config().storage_buffer_bytes = 0;
        node.on_create([this](ayni_kernel_t *kernel) {
            auto principal = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &principal), AYNI_OK);
            auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            auto type = goal_type_desc("context", registered_version, nullptr, nullptr,
                                       AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE));
            type.context_mode = registered_context_mode;
            type.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
            if (require_approval) {
                const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("observe"), 0, 0, nullptr};
                ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
                type.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
                type.export_requirements = [](void *, ayni_bytes_v1,
                                              const ayni_goal_requirement_sink_v1 *sink) {
                    const ayni_goal_requirement_v1 need{
                        sizeof(need), 1, text("observe"), {sizeof(ayni_scope_v1), 0, nullptr}};
                    return sink->emit(sink->ctx, &need);
                };
            }
            ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
            auto policy = strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &strategy, 1, 8);
            ASSERT_EQ(ayni_register_strategy(kernel, &policy), AYNI_OK);
        });
        node.run();
        open_session();
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        ASSERT_EQ(ayni_get_agent(node.kernel(), agent_a, &agent), AYNI_OK);
        owner = agent.instance_id;
        strategy.answer = [](const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &out) {
            const uint8_t body[] = {'a', 'n', 's', 'w', 'e', 'r'};
            std::memcpy(out.checkpoint, body, sizeof(body));
            out.checkpoint_size = sizeof(body);
            return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
        };
    }
    void open_session() {
        auto desc = session_desc();
        ASSERT_EQ(ayni_open_session(node.kernel(), &desc, &session), AYNI_OK);
    }
    void pump(unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            node.sim().advance_us(10000);
            node.step();
            node.sim().complete_storage();
        }
    }
    void durable(uint64_t seq) {
        for (unsigned i = 0; i < 200 && node.info().durable_seq < seq; ++i) {
            pump();
        }
        ASSERT_GE(node.info().durable_seq, seq);
    }
    Submitted submit(ayni_intent_kind_t kind, const void *body, uint32_t revision = 0,
                     uint64_t command = 0) {
        Submitted out{};
        out.result.struct_size = sizeof(out.result);
        out.error.struct_size = sizeof(out.error);
        const ayni_intent_v1 intent{sizeof(intent),
                                    kind,
                                    command == 0 ? next_command++ : command,
                                    session,
                                    revision,
                                    0,
                                    body};
        out.status = ayni_submit_intent(node.kernel(), &intent, &out.result, &out.error);
        return out;
    }
    ayni_context_info_v1 context() {
        ayni_context_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_context(node.kernel(), session, owner, &out), AYNI_OK);
        return out;
    }
    Submitted open() {
        ayni_context_command_v1 body{};
        body.struct_size = sizeof(body);
        body.owner_instance_id = owner;
        return submit(AYNI_INTENT_CONTEXT_OPEN, &body);
    }
    Submitted change(ayni_intent_kind_t kind, const ayni_context_info_v1 &info, uint64_t task = 0,
                     ayni_context_action_t action = AYNI_CONTEXT_IDLE, uint64_t turn = 0) {
        ayni_context_command_v1 body{sizeof(body),    action, 0,   info.context_id,
                                     info.generation, task,   turn};
        return submit(kind, &body, info.revision);
    }
    Submitted create(ayni_bytes_v1 parameters = {nullptr, 0}) {
        const auto body =
            WorkNode::create_body(owner, 0, "context", 1, parameters, AYNI_TRIGGER_ONCE);
        return submit(AYNI_INTENT_GOAL_CREATE, &body);
    }
    uint64_t complete(ayni_bytes_v1 parameters = {nullptr, 0}) {
        const auto created = create(parameters);
        EXPECT_EQ(created.status, AYNI_OK);
        if (created.status != AYNI_OK) {
            return 0;
        }
        ayni_goal_info_v1 goal{};
        goal.struct_size = sizeof(goal);
        ayni_task_info_v1 task{};
        task.struct_size = sizeof(task);
        for (unsigned i = 0; i < 200; ++i) {
            pump();
            EXPECT_EQ(ayni_get_goal(node.kernel(), created.result.object.id, &goal), AYNI_OK);
            if (goal.last_task_id == 0) {
                continue;
            }
            EXPECT_EQ(ayni_get_task(node.kernel(), goal.last_task_id, &task), AYNI_OK);
            if (task.lifecycle == AYNI_TASK_COMPLETED) {
                durable(task.commit_seq);
                return task.task_id;
            }
        }
        ADD_FAILURE() << "context Task did not complete";
        return 0;
    }
    void reopen() {
        node.reboot();
        node.recover();
        ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        open_session();
    }
};
// White-box, C-STORE-007/009 / ADR-0060: a recovered anchor must have an existing
// owner and lie inside the frozen snapshot boundary; duplicate rows are corrupt.
TEST_F(KernelContext, snapshot_context_rejects_invalid_history_missing_owner_and_duplicate_anchor) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto original = snapshot_model(node);
    ASSERT_EQ(restore_model(original), ayni::durability::BatchCheck::ok);
    std::size_t target = original.records.size();
    for (std::size_t at = 0; at < original.records.size(); ++at) {
        if (original.records[at].header.type == ayni::knowledge::context_record_type) {
            target = at;
        }
    }
    ASSERT_LT(target, original.records.size());
    for (unsigned scenario = 0; scenario < 10; ++scenario) {
        SCOPED_TRACE(scenario);
        auto candidate = original;
        auto &row = candidate.records[target];
        ayni::knowledge::ContextRecord fact{};
        ASSERT_TRUE(ayni::knowledge::read_context_record(row.body.data(), row.header.length, fact));
        if (scenario == 0) {
            fact.stamp.commit_seq = 0;
        } else if (scenario == 1) {
            fact.stamp.transition_id = 0;
        } else if (scenario == 2) {
            fact.stamp.commit_seq = candidate.metadata.covers_commit_seq + 1;
        } else if (scenario == 3) {
            fact.stamp.transition_id = candidate.metadata.max_transition_id + 1;
        } else if (scenario == 4 || scenario == 5) {
            ayni::foundation::IdAllocator allocator;
            allocator.start(candidate.metadata.boot_epoch + 1);
            uint64_t absent = 0;
            ASSERT_TRUE(allocator.allocate(absent));
            (scenario == 4 ? fact.id : fact.owner_instance_id) = absent;
        } else if (scenario == 6) {
            fact.revision = 0;
        } else if (scenario == 7) {
            fact.pending.action = static_cast<ayni::knowledge::ContextAction>(UINT32_MAX);
        } else if (scenario == 8) {
            candidate.records.insert(
                candidate.records.begin() + static_cast<std::ptrdiff_t>(target), row);
            EXPECT_EQ(restore_model(candidate), ayni::durability::BatchCheck::corrupt);
            continue;
        } else {
            row.header.length = 1;
            EXPECT_EQ(restore_model(candidate), ayni::durability::BatchCheck::corrupt);
            continue;
        }
        std::vector<uint8_t> bytes(row.body.size());
        ayni::knowledge::write_context_record(fact, bytes.data());
        row.body = std::move(bytes);
        EXPECT_EQ(restore_model(candidate), ayni::durability::BatchCheck::corrupt);
    }
    Node disabled;
    disabled.config() = original.config;
    disabled.config().memory_capacity = 0;
    EXPECT_EQ(restore_model(original, disabled), ayni::durability::BatchCheck::no_room);
}

// C-MEM-009 / C-STORE-007/009: OPEN has one exact initial anchor shape and
// owner. A CRC-valid changed fact cannot be justified by the unchanged receipt.
TEST_F(KernelContext, changed_initial_anchor_bits_cannot_substitute_another_open_result) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto snapshot = snapshot_model(node);
    ASSERT_EQ(restore_model(snapshot), ayni::durability::BatchCheck::ok);
    std::size_t row_index = snapshot.records.size();
    for (std::size_t at = 0; at < snapshot.records.size(); ++at) {
        if (snapshot.records[at].header.type == ayni::knowledge::context_record_type) {
            row_index = at;
        }
    }
    ASSERT_LT(row_index, snapshot.records.size());
    constexpr unsigned bits_per_byte = 8;
    // records.md type26/v1: the final fixed fields are the two-u64 stamp.
    // Snapshot commands retain commit_seq, not the original transition ID.
    constexpr uint32_t transition_at =
        ayni::knowledge::context_record_fixed_bytes - ayni::kernel::snapshot_stamp_size;
    const auto bytes = snapshot.records[row_index].body.size();
    for (std::size_t at = 0; at < bytes; ++at) {
        if (at >= transition_at && at < transition_at + sizeof(uint64_t)) {
            continue;
        }
        for (unsigned bit = 0; bit < bits_per_byte; ++bit) {
            SCOPED_TRACE(::testing::Message() << at << ':' << bit);
            auto candidate = snapshot;
            candidate.records[row_index].body[at] ^= static_cast<uint8_t>(1U << bit);
            EXPECT_EQ(restore_model(candidate), ayni::durability::BatchCheck::corrupt);
        }
    }
    const auto log = node.sim().log();
    const auto configure_recovery = [&](Node &target) {
        target.config() = snapshot.config;
        target.on_create([](ayni_kernel_t *kernel) {
            const auto principal = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &principal), AYNI_OK);
        });
    };
    Node control;
    configure_recovery(control);
    control.sim().log() = log;
    control.create_and_start();
    control.recover();
    ASSERT_EQ(control.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    std::size_t batch_index = log.size();
    JournalBatch batch{};
    std::size_t record_index = 0;
    for (std::size_t at = 0; at < log.size(); ++at) {
        auto decoded = read_stored_batch(log[at]);
        for (std::size_t item = 0; item < decoded.records.size(); ++item) {
            if (decoded.records[item].header.type == ayni::knowledge::context_record_type) {
                batch_index = at;
                record_index = item;
                batch = std::move(decoded);
                break;
            }
        }
    }
    ASSERT_LT(batch_index, log.size());
    for (std::size_t at = 0; at < batch.records[record_index].body.size(); ++at) {
        for (unsigned bit = 0; bit < bits_per_byte; ++bit) {
            SCOPED_TRACE(::testing::Message() << at << ':' << bit);
            auto changed = batch;
            changed.records[record_index].body[at] ^= static_cast<uint8_t>(1U << bit);
            auto candidate = log;
            candidate[batch_index] = write_stored_batch(changed);
            Node recovered;
            configure_recovery(recovered);
            recovered.sim().log() = std::move(candidate);
            recovered.create_and_start();
            recovered.recover();
            EXPECT_EQ(recovered.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
            EXPECT_EQ(recovered.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
            EXPECT_EQ(ayni::kernel::KernelTestProbe::work(recovered.kernel()).contexts.size(), 0U);
        }
    }
}

TEST_F(KernelContext, explicit_open_is_required_and_retries_keep_one_anchor) {
    EXPECT_EQ(create().status, AYNI_NOT_READY);
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    EXPECT_EQ(open().status, AYNI_INVALID_STATE);
    durable(opened.result.commit_seq);
    const auto current = context();
    EXPECT_EQ(current.generation, 0U);
    EXPECT_EQ(current.revision, 1U);
    EXPECT_EQ(current.pending_action, AYNI_CONTEXT_IDLE);
    reopen();
    EXPECT_EQ(context().context_id, current.context_id);
}
TEST_F(KernelContext, publication_waits_for_ack_and_old_task_cannot_publish_again) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto task = complete();
    ASSERT_NE(task, 0U);
    const auto publication =
        change(AYNI_INTENT_CONTEXT_PUBLISH, context(), task, AYNI_CONTEXT_IDLE, 1);
    ASSERT_EQ(publication.status, AYNI_OK);
    durable(publication.result.commit_seq);
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_PUBLISH);
    EXPECT_EQ(create().status, AYNI_BUSY);
    // A normal reopen preserves the exact pending Task and file target, never adopts old text.
    reopen();
    EXPECT_EQ(context().pending_task_id, task);
    const auto ack = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(ack.status, AYNI_OK);
    durable(ack.result.commit_seq);
    EXPECT_EQ(change(AYNI_INTENT_CONTEXT_PUBLISH, context(), task, AYNI_CONTEXT_IDLE, 2).status,
              AYNI_EXPIRED);
    EXPECT_NE(complete(), 0U);
}
TEST_F(KernelContext, forget_immediately_blocks_old_checkpoint_and_waits_for_exact_file_ack) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto task = complete();
    ASSERT_NE(task, 0U);
    ayni_task_info_v1 completed{};
    completed.struct_size = sizeof(completed);
    ASSERT_EQ(ayni_get_task(node.kernel(), task, &completed), AYNI_OK);
    const auto old = context();
    const auto forgotten = change(AYNI_INTENT_CONTEXT_FORGET, old, 0, AYNI_CONTEXT_ERASE_ALL);
    ASSERT_EQ(forgotten.status, AYNI_OK);
    std::array<uint8_t, 8> bytes{};
    uint32_t written = 0;
    EXPECT_EQ(ayni_copy_task_checkpoint(node.kernel(), task, bytes.data(), bytes.size(), &written),
              AYNI_EXPIRED);
    written = 99;
    bytes.fill(0x5a);
    EXPECT_EQ(ayni_get_goal_parameters(node.kernel(), completed.goal_id, bytes.data(), bytes.size(),
                                       &written),
              AYNI_EXPIRED);
    EXPECT_EQ(written, 99U);
    EXPECT_EQ(bytes.front(), 0x5a);
    durable(forgotten.result.commit_seq);
    pump(100);
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_ERASE_ALL);
    EXPECT_EQ(context().revision, old.revision + 1);
    EXPECT_EQ(change(AYNI_INTENT_CONTEXT_ACK, old).status, AYNI_REVISION_CONFLICT);
    reopen();
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_ERASE_ALL);
    const auto ack = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(ack.status, AYNI_OK);
    durable(ack.result.commit_seq);
    pump(600);
    reopen();
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_IDLE);
    EXPECT_EQ(ayni_copy_task_checkpoint(node.kernel(), task, bytes.data(), bytes.size(), &written),
              AYNI_EXPIRED);
    EXPECT_NE(complete(), 0U);
}
// ADR-0060: a command receipt owns its source evidence independently of the Goal retention window.
TEST_F(KernelContext, retired_goal_receipt_expires_when_its_original_context_is_forgotten) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto original_command = next_command;
    const auto task = complete();
    ASSERT_NE(task, 0U);
    ayni_task_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_task(node.kernel(), task, &info), AYNI_OK);
    for (unsigned round = 0; round < 3; ++round) {
        ASSERT_NE(complete(), 0U);
    }
    ayni_goal_info_v1 goal{};
    goal.struct_size = sizeof(goal);
    EXPECT_EQ(ayni_get_goal(node.kernel(), info.goal_id, &goal), AYNI_NOT_FOUND);
    const auto body =
        WorkNode::create_body(owner, 0, "context", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE);
    EXPECT_EQ(submit(AYNI_INTENT_GOAL_CREATE, &body, 0, original_command).status, AYNI_OK);
    reopen();
    EXPECT_EQ(submit(AYNI_INTENT_GOAL_CREATE, &body, 0, original_command).status, AYNI_OK);
    const auto forgotten = change(AYNI_INTENT_CONTEXT_FORGET, context(), 0, AYNI_CONTEXT_ERASE_ALL);
    ASSERT_EQ(forgotten.status, AYNI_OK);
    durable(forgotten.result.commit_seq);
    pump(100);
    const auto ack = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(ack.status, AYNI_OK);
    durable(ack.result.commit_seq);
    pump(600);
    EXPECT_EQ(submit(AYNI_INTENT_GOAL_CREATE, &body, 0, original_command).status, AYNI_EXPIRED);
    reopen();
    EXPECT_EQ(submit(AYNI_INTENT_GOAL_CREATE, &body, 0, original_command).status, AYNI_EXPIRED);
}
// ADR-0060: the anchor carries the full Task source set, never only its own revision token.
TEST_F(KernelContext,
       deleting_memory_invalidates_published_context_and_requires_derived_file_erasure) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    ayni_memory_save_v1 value{};
    value.struct_size = sizeof(value);
    value.name_space = AYNI_MEMORY_PRIVATE;
    value.namespace_id = owner;
    value.body = text("tea");
    const auto saved = submit(AYNI_INTENT_MEMORY_SAVE, &value);
    ASSERT_EQ(saved.status, AYNI_OK);
    durable(saved.result.commit_seq);
    strategy.answer = [&](const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &out) {
        std::array<char, 64> body{};
        ayni_memory_info_v1 metadata{};
        metadata.struct_size = sizeof(metadata);
        uint32_t size = 0;
        EXPECT_EQ(snapshot.memory->read(snapshot.memory->context, saved.result.object.id,
                                        AYNI_MEMORY_CONTEXT, &metadata, body.data(), body.size(),
                                        &size),
                  AYNI_OK);
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
    };
    const auto task = complete();
    ASSERT_NE(task, 0U);
    const auto published =
        change(AYNI_INTENT_CONTEXT_PUBLISH, context(), task, AYNI_CONTEXT_IDLE, 1);
    ASSERT_EQ(published.status, AYNI_OK);
    durable(published.result.commit_seq);
    const auto ack = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(ack.status, AYNI_OK);
    durable(ack.result.commit_seq);
    reopen();
    const auto inherited = create();
    ASSERT_EQ(inherited.status, AYNI_OK);
    durable(inherited.result.commit_seq);
    const ayni_memory_target_v1 target{sizeof(target), 0, saved.result.object.id};
    const auto removed = submit(AYNI_INTENT_MEMORY_DELETE, &target, 1);
    ASSERT_EQ(removed.status, AYNI_OK);
    // Both Goal slots are retained by pending privacy cleanup; this admission fails on capacity
    // before consulting sources. The existing Goal export below checks immediate invalidation.
    EXPECT_EQ(create().status, AYNI_CAPACITY_EXCEEDED);
    uint8_t retained = 0x5a;
    uint32_t copied = 99;
    EXPECT_EQ(
        ayni_get_goal_parameters(node.kernel(), inherited.result.object.id, &retained, 1, &copied),
        AYNI_EXPIRED);
    EXPECT_EQ(retained, 0x5a);
    EXPECT_EQ(copied, 99U);
    pump(100);
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_ERASE_DERIVED);
    EXPECT_EQ(context().last_turn_id, 1U);
    const auto erased = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(erased.status, AYNI_OK);
    durable(erased.result.commit_seq);
    pump(600);
    reopen();
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_IDLE);
    EXPECT_EQ(context().last_turn_id, 1U);
}

class KernelContextApproval : public KernelContext {
    void SetUp() override {
        require_approval = true;
        KernelContext::SetUp();
    }
};
// ADR-0060: an approval's private canonical echo outlives its Goal, so the command itself must
// retain lineage and participate in logical arena and both-slot erasure.
TEST_F(KernelContextApproval, retired_approval_canonical_is_erased_with_its_context) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const std::string secret = "retired-private-approval-parameters";
    const auto body = WorkNode::create_body(
        owner, 0, "context", 1,
        {reinterpret_cast<const uint8_t *>(secret.data()), static_cast<uint32_t>(secret.size())},
        AYNI_TRIGGER_ONCE);
    const auto goal = submit(AYNI_INTENT_GOAL_CREATE, &body);
    ASSERT_EQ(goal.status, AYNI_OK);
    durable(goal.result.commit_seq);
    ApprovalRequest approval(node.kernel(), goal.result.object.id);
    const auto approval_command = next_command;
    const auto accepted = submit(AYNI_INTENT_GOAL_APPROVE, &approval.body, approval.info.revision);
    ASSERT_EQ(accepted.status, AYNI_OK);
    durable(accepted.result.commit_seq);
    pump(50);
    const ayni_grant_create_v1 global{sizeof(global),
                                      1,
                                      text("observe"),
                                      owner,
                                      0,
                                      AYNI_GRANT_PERMANENT,
                                      0,
                                      0,
                                      {sizeof(ayni_scope_v1), 0, nullptr}};
    const auto granted = submit(AYNI_INTENT_GRANT_CREATE, &global);
    ASSERT_EQ(granted.status, AYNI_OK);
    durable(granted.result.commit_seq);
    for (unsigned round = 0; round < 3; ++round) {
        ASSERT_NE(complete(), 0U);
    }
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(node.kernel(), goal.result.object.id, &info), AYNI_NOT_FOUND);
    const auto before = node.arena_bytes();
    EXPECT_NE(std::search(before.data, before.data + before.size, secret.begin(), secret.end()),
              before.data + before.size);
    reopen();
    EXPECT_EQ(
        submit(AYNI_INTENT_GOAL_APPROVE, &approval.body, approval.info.revision, approval_command)
            .status,
        AYNI_OK);
    const auto forgotten = change(AYNI_INTENT_CONTEXT_FORGET, context(), 0, AYNI_CONTEXT_ERASE_ALL);
    ASSERT_EQ(forgotten.status, AYNI_OK);
    durable(forgotten.result.commit_seq);
    pump(100);
    const auto ack = change(AYNI_INTENT_CONTEXT_ACK, context());
    ASSERT_EQ(ack.status, AYNI_OK);
    durable(ack.result.commit_seq);
    pump(600);
    const auto arena = node.arena_bytes();
    EXPECT_EQ(std::search(arena.data, arena.data + arena.size, secret.begin(), secret.end()),
              arena.data + arena.size);
    for (const auto &snapshot : node.sim().durable_storage().snapshots) {
        ASSERT_FALSE(snapshot.empty());
        EXPECT_EQ(std::search(snapshot.begin(), snapshot.end(), secret.begin(), secret.end()),
                  snapshot.end());
    }
    for (const auto &batch : node.sim().durable_storage().log) {
        EXPECT_EQ(std::search(batch.bytes.begin(), batch.bytes.end(), secret.begin(), secret.end()),
                  batch.bytes.end());
    }
    EXPECT_EQ(
        submit(AYNI_INTENT_GOAL_APPROVE, &approval.body, approval.info.revision, approval_command)
            .status,
        AYNI_EXPIRED);
    reopen();
    EXPECT_EQ(
        submit(AYNI_INTENT_GOAL_APPROVE, &approval.body, approval.info.revision, approval_command)
            .status,
        AYNI_EXPIRED);
}
// ADR-0060, White-box: a later Context publication can invalidate an earlier scanned Memory.
// This checks the deterministic owner sweep only; external file/slot gates have separate tests.
TEST_F(KernelContext, context_erasure_revisits_earlier_memory_until_sources_are_stable) {
    using namespace ayni::knowledge;
    auto &model = ayni::kernel::KernelTestProbe::work(node.kernel());
    auto allocate = [&] {
        uint64_t id = 0;
        EXPECT_TRUE(model.ids.allocate(id));
        return id;
    };
    const auto context_a = allocate(), context_b = allocate();
    model.contexts.create(context_a, owner, model.transitions.next());
    model.contexts.create(context_b, allocate(), model.transitions.next());
    const auto actor_b = model.contexts.at(1).owner_instance_id;
    auto context_ref = [&](uint64_t id, uint64_t actor) {
        return MemoryDependency{DependencyKind::context, id, 1, actor,
                                MemoryPurpose::context,  {}, {}};
    };
    auto memory_ref = [&](uint64_t id) {
        return MemoryDependency{DependencyKind::memory, id, 1, owner,
                                MemoryPurpose::context, {}, {}};
    };
    auto add = [&](MemoryDependencies deps) {
        MemoryValue value{MemoryNamespace::private_agent,
                          owner,
                          deps.count ? MemorySource::derived : MemorySource::user,
                          ProfileField::none,
                          deps.count == 0,
                          0,
                          text("retained-private-text"),
                          {},
                          deps};
        MemoryWritePlan plan{};
        EXPECT_EQ(model.memories.check_write(0, 0, value, plan), AYNI_OK);
        const auto id = allocate();
        model.memories.install(plan, id, value, model.transitions.next());
        return id;
    };
    MemoryDependencies sources{};
    sources.items[sources.count++] = context_ref(context_b, actor_b);
    const auto early = add(sources);
    sources = {};
    sources.items[sources.count++] = context_ref(context_a, owner);
    const auto bridge = add(sources);
    sources.items[sources.count++] = memory_ref(bridge);
    model.contexts.publish(1, allocate(), sources, model.transitions.next());
    model.contexts.acknowledge(1, model.transitions.next());
    const auto original = add({});
    sources = {};
    sources.items[sources.count++] = memory_ref(original);
    model.contexts.publish(0, allocate(), sources, model.transitions.next());
    model.contexts.acknowledge(0, model.transitions.next());
    uint32_t index = 0;
    ASSERT_TRUE(model.memories.find(original, index));
    const auto deleted = model.transitions.next();
    model.memories.tombstone(index, deleted);
    model.privacy.request(deleted);
    bool rescanned = false;
    for (unsigned step = 0;
         step < 200 && model.privacy.record().phase != ayni::work::PrivacyPhase::files; ++step) {
        ayni::work::PrivacyRecord next{};
        ASSERT_TRUE(ayni::kernel::next_privacy_step(model, 0, next));
        if (model.privacy.source_rescan_required()) {
            EXPECT_EQ(next.phase, ayni::work::PrivacyPhase::memories);
            EXPECT_EQ(next.cursor, 0U);
            rescanned = true;
        }
        next.stamp = model.transitions.next();
        ASSERT_TRUE(model.privacy.can_restore(next));
        ASSERT_TRUE(ayni::kernel::apply_privacy_step(model, next, model.ids.boot_epoch()));
    }
    ASSERT_TRUE(rescanned);
    EXPECT_EQ(model.privacy.record().phase, ayni::work::PrivacyPhase::files);
    for (const auto id : {early, bridge, original}) {
        ASSERT_TRUE(model.memories.find(id, index));
        EXPECT_EQ(model.memories.at(index).state, MemoryState::scrubbed);
        EXPECT_EQ(model.memories.body(index).size, 0U);
    }
}
// C-STORE-011: a smaller startup dependency budget is capacity, never record corruption.
TEST(ContextCapacity, valid_history_exceeding_configured_dependencies_is_classified_as_capacity) {
    using namespace ayni::knowledge;
    ayni::foundation::IdAllocator ids;
    ids.start(1);
    auto allocate = [&] {
        uint64_t id = 0;
        EXPECT_TRUE(ids.allocate(id));
        return id;
    };
    ContextRecord record{};
    record.id = allocate();
    record.owner_instance_id = allocate();
    record.stamp = {1, 1};
    for (uint32_t i = 0; i < 2; ++i) {
        record.dependencies.items[record.dependencies.count++] = {
            DependencyKind::memory, allocate(), 1, record.owner_instance_id,
            MemoryPurpose::context, {},         {}};
    }
    std::array<ContextRecord, 1> context_storage{};
    ContextTable contexts(context_storage.data(), 1);
    EXPECT_EQ(contexts.restore(record, 1), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(contexts.size(), 0U);
    std::array<ayni::work::ProvenanceRecord, 1> provenance_storage{};
    ayni::work::ProvenanceTable provenance(provenance_storage.data(), {1, 0});
    ayni::work::ProvenanceRecord source{};
    source.kind = ayni::work::ProvenanceKind::goal;
    source.id = allocate();
    source.content = ayni::work::ContentState::available;
    source.dependencies = record.dependencies;
    source.stamp = {1, 1};
    EXPECT_EQ(provenance.restore(0, source, 1), AYNI_CAPACITY_EXCEEDED);
    source.kind = static_cast<ayni::work::ProvenanceKind>(99);
    EXPECT_EQ(provenance.restore(0, source, 1), AYNI_INVALID_ARGUMENT);
}
// C-MEM-005: ordinary owner exports cannot bypass source capture from inside a policy callback.
TEST_F(KernelContext, policy_callback_cannot_export_another_goals_or_tasks_private_body) {
    const auto opened = open();
    ASSERT_EQ(opened.status, AYNI_OK);
    durable(opened.result.commit_seq);
    const auto first = complete();
    ASSERT_NE(first, 0U);
    ayni_task_info_v1 task{};
    task.struct_size = sizeof(task);
    ASSERT_EQ(ayni_get_task(node.kernel(), first, &task), AYNI_OK);
    bool checked = false;
    strategy.answer = [&](const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &out) {
        uint8_t body = 0x5a;
        uint32_t size = 99;
        EXPECT_EQ(ayni_get_goal_parameters(node.kernel(), task.goal_id, &body, 1, &size),
                  AYNI_INVALID_STATE);
        EXPECT_EQ(ayni_copy_task_checkpoint(node.kernel(), first, &body, 1, &size),
                  AYNI_INVALID_STATE);
        EXPECT_EQ(body, 0x5a);
        EXPECT_EQ(size, 99U);
        checked = true;
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
    };
    EXPECT_NE(complete(), 0U);
    EXPECT_TRUE(checked);
}
class KernelLegacyContext : public KernelContext {
    void SetUp() override {
        registered_context_mode = AYNI_GOAL_CONTEXT_NONE;
        KernelContext::SetUp();
    }
};
// ADR-0060: a new trusted context version cannot adopt untracked content from the old version.
TEST_F(KernelLegacyContext, changed_type_version_rejects_untracked_historical_body_exports) {
    const uint8_t original[] = {'o', 'l', 'd'};
    const auto task_id = complete({original, sizeof(original)});
    ASSERT_NE(task_id, 0U);
    ayni_task_info_v1 task{};
    task.struct_size = sizeof(task);
    ASSERT_EQ(ayni_get_task(node.kernel(), task_id, &task), AYNI_OK);
    uint8_t output[8]{};
    uint32_t copied = 0;
    ASSERT_EQ(
        ayni_get_goal_parameters(node.kernel(), task.goal_id, output, sizeof(output), &copied),
        AYNI_OK);
    EXPECT_EQ(copied, sizeof(original));
    registered_version = 2;
    registered_context_mode = AYNI_GOAL_CONTEXT_AGENT;
    reopen();
    copied = 99;
    EXPECT_EQ(
        ayni_get_goal_parameters(node.kernel(), task.goal_id, output, sizeof(output), &copied),
        AYNI_EXPIRED);
    EXPECT_EQ(ayni_copy_task_checkpoint(node.kernel(), task_id, output, sizeof(output), &copied),
              AYNI_EXPIRED);
    EXPECT_EQ(copied, 99U);
    EXPECT_EQ(output[0], original[0]);
}
} // namespace
