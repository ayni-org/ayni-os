// C-STORE-006/007/009/010: publication is a durable state boundary, and
// neither a candidate nor a damaged active slot can roll recovery backward.
#include "durability/snapshot/snapshot_codec.h"
#include "foundation/crc32.h"
#include "foundation/little_endian.h"
#include "work_node.h"

#include <gtest/gtest.h>

namespace {
using namespace ayni::test;
using ayni::hosts::sim::CutAppend;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StorageImage;
using ayni::hosts::sim::StoragePort;

void configure(WorkNode &work) {
    work.node().config().storage_buffer_bytes = 0;
    work.node().config().storage_log_max_bytes = 32768;
}

ayni_step_report_v1 step(WorkNode &work, bool complete = true) {
    const auto report = work.node().step(1);
    if (complete) {
        work.node().sim().complete_storage();
    }
    if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
        report.next_deadline_us != UINT64_MAX &&
        report.next_deadline_us > work.node().sim().now_us()) {
        work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
    }
    return report;
}

bool drain(WorkNode &work, bool stop_at_maintenance = false) {
    for (int round = 0; round < 10000; ++round) {
        step(work);
        if (stop_at_maintenance && work.kernel()->maintenance.active()) {
            return true;
        }
        if (work.node().lifecycle() != AYNI_LIFECYCLE_RUNNING) {
            return false;
        }
        if (work.kernel()->log.idle() && work.kernel()->completions.in_flight() == 0U &&
            !work.kernel()->maintenance.active() && work.kernel()->storage_has_room()) {
            return false;
        }
    }
    ADD_FAILURE() << "maintenance did not settle";
    return false;
}

struct Image {
    StorageImage storage;
    uint64_t goal_id = 0;
    uint32_t revision = 0;
    uint32_t active_slot = 0;
};

Image maintenance_source() {
    WorkNode work;
    configure(work);
    work.run();
    for (int index = 0; index < 1000; ++index) {
        const auto created =
            work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
        if (created.status != AYNI_OK) {
            ADD_FAILURE() << "fixture goal rejected: " << created.status;
            return {};
        }
        const uint64_t id = created.result.object.id;
        if (drain(work, true)) {
            return {work.node().sim().durable_storage(), id, work.goal(id).revision};
        }
        const auto cancelled = work.change(AYNI_INTENT_GOAL_CANCEL, id, work.goal(id).revision);
        if (cancelled.status != AYNI_OK) {
            ADD_FAILURE() << "fixture cancellation rejected: " << cancelled.status;
            return {};
        }
        if (drain(work, true)) {
            return {work.node().sim().durable_storage(), id, work.goal(id).revision};
        }
    }
    ADD_FAILURE() << "log never requests a snapshot";
    return {};
}

void recover(WorkNode &work, const Image &image) {
    configure(work);
    work.node().sim().durable_storage() = image.storage;
    work.node().create_and_start();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(work.goal(image.goal_id).revision, image.revision);
}

bool wait_for_request(WorkNode &work, unsigned ordinal) {
    unsigned seen = 0;
    for (int round = 0; round < 10000; ++round) {
        const auto report = work.node().step(1);
        if (work.kernel()->maintenance.active() && work.node().sim().pending_storage() != 0U) {
            if (++seen == ordinal) {
                return true;
            }
        }
        work.node().sim().complete_storage();
        if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
            report.next_deadline_us != UINT64_MAX &&
            report.next_deadline_us > work.node().sim().now_us()) {
            work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
        }
        if (work.kernel()->maintenance.done()) {
            return false;
        }
    }
    return false;
}

Image activated_image() {
    const Image source = maintenance_source();
    WorkNode work;
    recover(work, source);
    drain(work);
    for (int index = 0; index < 1000; ++index) {
        const auto created =
            work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
        if (created.status != AYNI_OK) {
            ADD_FAILURE() << "fixture goal rejected: " << created.status;
            return {};
        }
        const uint64_t id = created.result.object.id;
        drain(work);
        if (work.kernel()->maintenance.metadata().generation >= 2U) {
            return {work.node().sim().durable_storage(), id, work.goal(id).revision,
                    work.kernel()->maintenance.slot()};
        }
        const auto cancelled = work.change(AYNI_INTENT_GOAL_CANCEL, id, work.goal(id).revision);
        if (cancelled.status != AYNI_OK) {
            ADD_FAILURE() << "fixture cancellation rejected: " << cancelled.status;
            return {};
        }
        drain(work);
        if (work.kernel()->maintenance.metadata().generation >= 2U) {
            return {work.node().sim().durable_storage(), id, work.goal(id).revision,
                    work.kernel()->maintenance.slot()};
        }
    }
    ADD_FAILURE() << "a second committed snapshot never appeared";
    return {};
}

TEST(SnapshotFaults, power_loss_at_each_publication_request_keeps_the_durable_work) {
    const Image source = maintenance_source();
    // This fixture fits its payload in one page: header, payload, header
    // readback, payload readback, marker, activation and trim are seven calls.
    for (unsigned request = 1; request <= 7; ++request) {
        for (const auto cut : {CutAppend::lost, CutAppend::stored, CutAppend::torn}) {
            SCOPED_TRACE(request);
            SCOPED_TRACE(static_cast<int>(cut));
            WorkNode work;
            recover(work, source);
            ASSERT_TRUE(wait_for_request(work, request));
            work.node().power_cut_and_start(cut);
            work.node().recover();
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
            EXPECT_EQ(work.goal(source.goal_id).revision, source.revision);
        }
    }
}

TEST(SnapshotFaults, a_complete_unactivated_candidate_does_not_select_its_state) {
    const Image source = maintenance_source();
    WorkNode work;
    recover(work, source);
    ASSERT_TRUE(wait_for_request(work, 6)); // marker durable, activation not stored
    auto &candidate = work.node().sim().durable_storage().snapshots[0];
    ASSERT_GT(candidate.size(), ayni::durability::snapshot_header_size);
    // Corrupting a candidate must not poison the still-authoritative journal.
    candidate[ayni::durability::snapshot_header_size] ^= 1U;
    work.node().power_cut_and_start(CutAppend::lost);
    work.node().recover();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.goal(source.goal_id).revision, source.revision);
}

TEST(SnapshotFaults, corrupt_or_unsupported_active_slot_never_falls_back_to_an_older_slot) {
    const Image active = activated_image();
    for (const bool unsupported : {false, true}) {
        SCOPED_TRACE(unsupported);
        auto image = active.storage;
        // The other slot contains the valid preceding generation.
        ASSERT_FALSE(image.snapshots[1U - active.active_slot].empty());
        auto &selected = image.snapshots[active.active_slot];
        if (unsupported) {
            ayni::foundation::store_le16(selected.data() + 4, 2);
            ayni::foundation::store_le32(selected.data() + 68,
                                         ayni::foundation::crc32(selected.data(), 68));
        } else {
            ayni::durability::SnapshotMetadata metadata{};
            ASSERT_EQ(ayni::durability::read_snapshot_header(
                          selected.data(), ayni::durability::snapshot_header_size, metadata),
                      ayni::durability::BatchCheck::ok);
            ASSERT_GT(metadata.payload_length, 0U);
            selected[ayni::durability::snapshot_header_size + metadata.payload_length - 1U] ^= 1U;
        }
        WorkNode work;
        configure(work);
        work.node().sim().durable_storage() = image;
        work.node().create_and_start();
        work.node().recover();
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(work.node().info().restriction,
                  unsupported ? AYNI_RESTRICTION_LOG_UNSUPPORTED : AYNI_RESTRICTION_LOG_CORRUPT);
        EXPECT_EQ(work.node().sim().log().size(), image.log.size());
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal(work.kernel(), active.goal_id, &info), AYNI_NOT_FOUND);
    }
}

TEST(SnapshotFaults, failed_candidate_never_deletes_the_valid_source_log) {
    const Image source = maintenance_source();
    WorkNode work;
    recover(work, source);
    const auto durable_source = work.node().sim().log();
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::write_snapshot, StorageFault::fail);
    }
    // Keep automatic reload from modifying the source after candidate failure.
    work.node().sim().inject_fault(StoragePort::fence, StorageFault::unavailable);
    for (int round = 0; round < 1000 && work.node().lifecycle() != AYNI_LIFECYCLE_RESTRICTED;
         ++round) {
        step(work);
    }
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    ASSERT_EQ(work.node().sim().log().size(), durable_source.size());
    for (std::size_t index = 0; index < durable_source.size(); ++index) {
        EXPECT_EQ(work.node().sim().log()[index].bytes, durable_source[index].bytes);
    }
}
} // namespace

namespace {
// White-box instrumentation observes the single write gate used by every
// mutating port, while application behavior and completions use the C ABI.
void rate_tick(WorkNode &work, uint64_t &last_deadline, unsigned &writes, unsigned &waits) {
    const auto report = work.node().step(1);
    const uint64_t deadline = work.kernel()->writes.deadline();
    if (deadline != last_deadline) {
        if (writes != 0U) {
            EXPECT_GE(deadline, last_deadline + 10000U);
        }
        last_deadline = deadline;
        ++writes;
    }
    work.node().sim().complete_storage();
    if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
        work.kernel()->completions.posted_count() == 0U && report.next_deadline_us != UINT64_MAX &&
        report.next_deadline_us > work.node().sim().now_us()) {
        const auto attempts = work.node().sim().storage_attempts().size();
        const auto waiting = work.node().step(1);
        EXPECT_EQ(waiting.items_done, 0U);
        EXPECT_EQ(waiting.more_ready, 0U);
        EXPECT_EQ(work.node().sim().storage_attempts().size(), attempts);
        EXPECT_EQ(work.kernel()->writes.deadline(), deadline);
        ++waits;
        work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
    }
}

void rate_drain(WorkNode &work, uint64_t &deadline, unsigned &writes, unsigned &waits) {
    for (int round = 0; round < 10000; ++round) {
        rate_tick(work, deadline, writes, waits);
        if (work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING && work.kernel()->log.idle() &&
            work.kernel()->completions.in_flight() == 0U && !work.kernel()->maintenance.active() &&
            work.kernel()->storage_has_room()) {
            return;
        }
    }
    ADD_FAILURE() << "write gate never becomes idle";
}

TEST(SnapshotFaults, every_mutating_phase_shares_the_interval_and_waits_without_spinning) {
    WorkNode work;
    configure(work);
    work.node().create_and_start();
    uint64_t deadline = 0;
    unsigned writes = 0;
    unsigned waits = 0;
    rate_drain(work, deadline, writes, waits);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    for (int index = 0; index < 1000 && work.kernel()->maintenance.metadata().generation == 0U;
         ++index) {
        const auto created =
            work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
        ASSERT_EQ(created.status, AYNI_OK);
        if (index == 0) {
            work.node().sim().inject_fault(StoragePort::append, StorageFault::unknown_lost);
        }
        rate_drain(work, deadline, writes, waits);
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, created.result.object.id,
                              work.goal(created.result.object.id).revision)
                      .status,
                  AYNI_OK);
        rate_drain(work, deadline, writes, waits);
    }
    EXPECT_GE(work.kernel()->maintenance.metadata().generation, 1U);
    EXPECT_GT(writes, 10U);
    EXPECT_GT(waits, 10U);
}
} // namespace

namespace {
TEST(SnapshotFaults, a_candidate_can_replace_a_superseded_slot_before_an_older_trim_finishes) {
    const auto source = maintenance_source();
    WorkNode work;
    recover(work, source);
    drain(work); // G1 activation and trim succeeded.
    uint64_t goal_id = 0;
    bool second_snapshot = false;
    for (int index = 0; index < 1000 && !second_snapshot; ++index) {
        const auto created =
            work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
        ASSERT_EQ(created.status, AYNI_OK);
        goal_id = created.result.object.id;
        second_snapshot = drain(work, true);
        if (!second_snapshot) {
            ASSERT_EQ(
                work.change(AYNI_INTENT_GOAL_CANCEL, goal_id, work.goal(goal_id).revision).status,
                AYNI_OK);
            second_snapshot = drain(work, true);
        }
    }
    ASSERT_TRUE(second_snapshot);
    ASSERT_TRUE(wait_for_request(work, 7)); // G2 activation durable, its trim pending.
    Image retained{work.node().sim().durable_storage(), goal_id, work.goal(goal_id).revision,
                   work.kernel()->maintenance.slot()};
    const uint32_t inactive = 1U - retained.active_slot;
    auto &candidate = retained.storage.snapshots[inactive];
    ayni::durability::SnapshotMetadata metadata{};
    ASSERT_EQ(ayni::durability::read_snapshot_header(
                  candidate.data(), ayni::durability::snapshot_header_size, metadata),
              ayni::durability::BatchCheck::ok);
    metadata.generation = 3;
    // A complete G3 candidate has no activation. G2 remains authoritative,
    // even though the retained log still begins with the obsolete G1 activation.
    ayni::durability::write_snapshot_header(metadata, candidate.data());
    ayni::durability::write_snapshot_marker(metadata, candidate.data() +
                                                          ayni::durability::snapshot_header_size +
                                                          metadata.payload_length);
    WorkNode restarted;
    recover(restarted, retained);
    EXPECT_EQ(restarted.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(restarted.goal(goal_id).revision, retained.revision);
}

ayni_submit_result_t too_small_without_a_larger_batch(void *, uint64_t, uint8_t *,
                                                      uint32_t capacity,
                                                      ayni_attempt_ref_v1 attempt,
                                                      ayni_completion_t *completion) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = attempt;
    result.outcome = AYNI_READ_BUFFER_TOO_SMALL;
    result.value = capacity; // A larger required size is missing.
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    return AYNI_SUBMIT_ACCEPTED;
}

ayni_submit_result_t too_small_from_snapshot(void *, uint32_t, uint64_t, uint8_t *,
                                             uint32_t capacity, ayni_attempt_ref_v1 attempt,
                                             ayni_completion_t *completion) {
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = attempt;
    result.outcome = AYNI_READ_BUFFER_TOO_SMALL; // Not a defined snapshot-read outcome.
    result.value = static_cast<uint64_t>(capacity) + 1U;
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    return AYNI_SUBMIT_ACCEPTED;
}

TEST(SnapshotFaults, malformed_log_buffer_requirement_is_a_host_read_failure) {
    Node node;
    node.replace_read_port(&too_small_without_a_larger_batch);
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_STORAGE_READ_FAILED);
}

TEST(SnapshotFaults, an_undefined_snapshot_buffer_requirement_is_a_host_read_failure) {
    const Image active = activated_image();
    Node fixture;
    fixture.sim().durable_storage() = active.storage;
    auto host = fixture.sim().table();
    host.storage_read_snapshot = &too_small_from_snapshot;
    std::size_t bytes = 0;
    std::size_t alignment = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&fixture.config(), &bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> memory(bytes / sizeof(uint64_t) + 1U);
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(ayni_create(&fixture.config(), &host, memory.data(), bytes, &kernel), AYNI_OK);
    ASSERT_EQ(ayni_start(kernel), AYNI_OK);
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    for (unsigned round = 0; round < 100; ++round) {
        ASSERT_EQ(ayni_step(kernel, 8, 1000000, nullptr), AYNI_OK);
        fixture.sim().complete_storage();
        ASSERT_EQ(ayni_get_kernel_info(kernel, &info), AYNI_OK);
        if (info.lifecycle == AYNI_LIFECYCLE_RESTRICTED) {
            break;
        }
    }
    EXPECT_EQ(info.lifecycle, AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(info.restriction, AYNI_RESTRICTION_STORAGE_READ_FAILED);
    EXPECT_EQ(ayni_begin_shutdown(kernel), AYNI_OK);
    EXPECT_EQ(ayni_step(kernel, 1, 1000000, nullptr), AYNI_OK);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}
} // namespace

namespace {
TEST(SnapshotFaults, retry_attempts_wait_exactly_ten_then_fifty_milliseconds_and_reopen_admission) {
    WorkNode work;
    configure(work);
    work.run();
    const auto body = WorkNode::create_body(work.instance(agent_a), 0, "folder");
    auto command = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    command.command_id = 100;
    const auto accepted = work.submit(command);
    ASSERT_EQ(accepted.status, AYNI_OK);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    const auto before = work.node().sim().storage_attempts().size();
    // White-box: advance only to the shared gate of the initial request.
    const auto initial_gate = work.kernel()->writes.deadline();
    if (initial_gate > work.node().sim().now_us()) {
        work.node().sim().advance_us(initial_gate - work.node().sim().now_us());
    }
    work.node().step(1);
    ASSERT_EQ(work.node().sim().storage_attempts().size(), before + 1U);
    const auto first_started = work.node().sim().now_us();
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    const auto first_failure = work.node().step(1);
    EXPECT_EQ(first_failure.next_deadline_us, first_started + 10000U);
    work.node().sim().advance_us(9999);
    EXPECT_EQ(work.node().step(1).items_done, 0U);
    EXPECT_EQ(work.node().sim().storage_attempts().size(), before + 1U);
    work.node().sim().advance_us(1);
    work.node().step(1);
    ASSERT_EQ(work.node().sim().storage_attempts().size(), before + 2U);
    const auto second_started = work.node().sim().now_us();
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    const auto second_failure = work.node().step(1);
    EXPECT_EQ(second_failure.next_deadline_us, second_started + 50000U);
    work.node().sim().advance_us(49999);
    EXPECT_EQ(work.node().step(1).items_done, 0U);
    EXPECT_EQ(work.node().sim().storage_attempts().size(), before + 2U);
    work.node().sim().advance_us(1);
    work.node().step(1);
    ASSERT_EQ(work.node().sim().storage_attempts().size(), before + 3U);
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    work.node().step(1);
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.node().info().durable_seq, accepted.result.commit_seq);
    command.command_id = 101;
    const auto later = work.submit(command);
    EXPECT_EQ(later.status, AYNI_OK);
    EXPECT_NE(later.result.object.id, accepted.result.object.id);
}
} // namespace

namespace {
TEST(SnapshotFaults, a_slow_strategy_cannot_spend_the_next_storage_interval_before_submit) {
    WorkNode work;
    configure(work);
    work.run();
    work.strategy().answer = [&work](const ayni_task_snapshot_v1 &snapshot,
                                     ayni_policy_output_v1 &output) {
        work.node().sim().advance_us(15000);
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
    };
    const auto body = WorkNode::create_body(work.instance(agent_a), 0, "timer", 1, {nullptr, 0},
                                            AYNI_TRIGGER_ONCE);
    ASSERT_EQ(work.create(body).status, AYNI_OK);
    auto gate = work.kernel()->writes.deadline();
    if (gate > work.node().sim().now_us()) {
        work.node().sim().advance_us(gate - work.node().sim().now_us());
    }
    // Deriving the task precedes its first decision. The creation batch is
    // in flight, and the task's own batch waits behind it.
    work.node().step();
    ASSERT_EQ(work.node().sim().pending_storage(), 1U);
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    gate = work.kernel()->writes.deadline();
    if (gate > work.node().sim().now_us()) {
        work.node().sim().advance_us(gate - work.node().sim().now_us());
    }
    const auto attempts_before = work.node().sim().storage_attempts().size();
    // This step takes 15 ms inside the strategy before it submits the
    // oldest queued journal batch to the Host.
    work.node().step();
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    ASSERT_EQ(work.node().sim().storage_attempts().size(), attempts_before + 1U);
    ASSERT_EQ(work.node().sim().pending_storage(), 1U);
    const auto actual_submit_time = work.node().sim().now_us();
    EXPECT_GE(work.kernel()->writes.deadline(), actual_submit_time + 10000U);
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    work.node().step();
    EXPECT_EQ(work.node().sim().storage_attempts().size(), attempts_before + 1U);
    EXPECT_EQ(work.node().sim().pending_storage(), 0U);
    work.node().sim().advance_us(9999);
    EXPECT_EQ(work.node().step().items_done, 0U);
    EXPECT_EQ(work.node().sim().storage_attempts().size(), attempts_before + 1U);
    work.node().sim().advance_us(1);
    work.node().step();
    EXPECT_EQ(work.node().sim().storage_attempts().size(), attempts_before + 2U);
    EXPECT_EQ(work.node().sim().pending_storage(), 1U);
}
} // namespace

namespace {
TEST(SnapshotFaults, an_unactivated_slot_read_failure_does_not_block_the_source_journal) {
    WorkNode work;
    work.node().sim().inject_fault(StoragePort::read_snapshot, StorageFault::fail);
    work.run();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder")).status,
              AYNI_OK);
}

// C-STORE-006/010: an activation is a REQUIRED, standalone control batch.
TEST(SnapshotFaults, optional_or_mixed_activation_cannot_authorize_a_storage_switch) {
    const Image active = activated_image();
    for (const bool mixed : {false, true}) {
        SCOPED_TRACE(mixed);
        auto image = active.storage;
        ayni::durability::BatchView batch{};
        ASSERT_EQ(ayni::durability::read_batch(image.log.front().bytes.data(),
                                               image.log.front().bytes.size(), batch),
                  ayni::durability::BatchCheck::ok);
        ayni::durability::RecordReader reader(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        ASSERT_EQ(reader.next(header, body), ayni::durability::RecordReader::Next::record);
        ASSERT_EQ(header.type, ayni::durability::snapshot_commit_record_type);
        std::vector<uint8_t> bytes(256);
        ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity, 0);
        header.flags = mixed ? ayni::durability::record_required : 0U;
        ASSERT_TRUE(writer.add_record(header, body));
        if (mixed) {
            ASSERT_TRUE(writer.add_record({999, 1, 0, 0}, nullptr));
        }
        bytes.resize(writer.finish());
        image.log.front().bytes = std::move(bytes);
        Node node;
        node.sim().durable_storage() = image;
        node.create_and_start();
        node.recover();
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
        EXPECT_EQ(node.sim().log().size(), image.log.size());
    }
}

TEST(SnapshotFaults, a_wrapped_activation_baseline_is_rejected_before_any_metadata_write) {
    std::array<uint8_t, ayni::durability::snapshot_commit_record_length> body{};
    ayni::durability::write_snapshot_activation({0, 0, 1, UINT64_MAX, 0}, body.data());
    std::vector<uint8_t> bytes(256);
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), {1, 0, 1});
    ASSERT_TRUE(writer.add_record({ayni::durability::snapshot_commit_record_type, 1,
                                   ayni::durability::record_required, body.size()},
                                  body.data()));
    bytes.resize(writer.finish());
    Node node;
    node.sim().log().push_back({0, bytes});
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    ASSERT_EQ(node.sim().log().size(), 1U);
    EXPECT_EQ(node.sim().log().front().bytes, bytes);
}
} // namespace
