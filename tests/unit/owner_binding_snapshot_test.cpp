// OWNER_BINDING survives both persistence views without replacing the
// Host's current expectation or granting legacy work execution rights.

#include "authority/principal/owner_binding_records.h"
#include "durability/batch/batch_codec.h"
#include "foundation/little_endian.h"
#include "kernel/persistence/work_replay.h"
#include "kernel/persistence/work_snapshot.h"
#include "work/agent/agent_records.h"
#include "work_node.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using ayni::kernel::KernelTestProbe;

namespace {

using namespace ayni::test;
using ayni::durability::BatchCheck;
using ayni::durability::RecordUse;
using ayni::kernel::WorkSnapshotReader;
using ayni::kernel::WorkSnapshotWriter;

constexpr std::size_t record_prefix = ayni::durability::record_header_size;
// Snapshot records append the original transition/commit pair (records.md).
constexpr uint32_t snapshot_stamp_size = 16;
constexpr uint32_t snapshot_commit_offset = 8;
constexpr std::size_t binding_bytes =
    record_prefix + ayni::authority::owner_binding_record_size + snapshot_stamp_size;

struct Snapshot {
    ayni::durability::SnapshotMetadata metadata{};
    std::vector<uint8_t> bytes;
    uint32_t pages = 0;
};

Snapshot snapshot_of(WorkNode &node, uint32_t page_size = 64) {
    Snapshot snapshot;
    const auto &kernel = *node.kernel();
    const auto layout = ayni::kernel::measure_snapshot(kernel.work);
    snapshot.metadata.generation = 1;
    snapshot.metadata.covers_commit_seq = kernel.work.transitions.latest().commit_seq;
    snapshot.metadata.max_transition_id = kernel.work.transitions.latest().transition_id;
    snapshot.metadata.boot_epoch = kernel.log.boot_epoch();
    snapshot.metadata.writer_epoch = kernel.log.writer_epoch();
    snapshot.metadata.payload_length = layout.payload_length;
    snapshot.metadata.record_count = layout.record_count;
    WorkSnapshotWriter writer(kernel.work);
    std::vector<uint8_t> page(page_size);
    for (unsigned round = 0; round < 64; ++round) {
        uint32_t size = 0;
        const auto result = writer.next(page.data(), page_size, size);
        if (result == WorkSnapshotWriter::Page::done) {
            EXPECT_EQ(snapshot.bytes.size(), layout.payload_length);
            return snapshot;
        }
        if (result == WorkSnapshotWriter::Page::no_room) {
            ADD_FAILURE() << "snapshot record exceeds the test page";
            return snapshot;
        }
        ++snapshot.pages;
        snapshot.bytes.insert(snapshot.bytes.end(), page.begin(), page.begin() + size);
    }
    ADD_FAILURE() << "snapshot writer did not finish";
    return snapshot;
}

BatchCheck import_snapshot(ayni::kernel::WorkModel &model, const Snapshot &snapshot) {
    WorkSnapshotReader reader(model, snapshot.metadata);
    std::size_t offset = 0;
    while (offset < snapshot.bytes.size()) {
        if (snapshot.bytes.size() - offset < record_prefix) {
            return BatchCheck::corrupt;
        }
        const auto header = ayni::durability::read_record_header(snapshot.bytes.data() + offset);
        offset += record_prefix;
        if (header.length > snapshot.bytes.size() - offset) {
            return BatchCheck::corrupt;
        }
        const auto result = reader.apply(header, snapshot.bytes.data() + offset);
        if (result != BatchCheck::ok) {
            return result;
        }
        offset += header.length;
    }
    return reader.complete();
}

// Keep the covered log as well as the activation, so discovery must still
// classify its historical headers before loading this valid slot.
void activate_snapshot(const Snapshot &snapshot, ayni::hosts::sim::StorageImage &image) {
    auto metadata = snapshot.metadata;
    metadata.payload_crc = ayni::durability::snapshot_crc32(
        snapshot.bytes.data(), static_cast<uint32_t>(snapshot.bytes.size()));
    auto &slot = image.snapshots[0];
    slot.resize(ayni::durability::snapshot_header_size + snapshot.bytes.size() +
                ayni::durability::snapshot_marker_size);
    ayni::durability::write_snapshot_header(metadata, slot.data());
    std::copy(snapshot.bytes.begin(), snapshot.bytes.end(),
              slot.begin() + ayni::durability::snapshot_header_size);
    ayni::durability::write_snapshot_marker(
        metadata, slot.data() + ayni::durability::snapshot_header_size + snapshot.bytes.size());
    std::array<uint8_t, ayni::durability::snapshot_commit_record_length> body{};
    ayni::durability::write_snapshot_activation({0, metadata.payload_crc, metadata.generation,
                                                 metadata.covers_commit_seq,
                                                 metadata.max_transition_id},
                                                body.data());
    std::vector<uint8_t> bytes(ayni::durability::snapshot_commit_batch_size);
    const ayni::durability::BatchIdentity identity{
        metadata.writer_epoch, metadata.covers_commit_seq + 1U, metadata.max_transition_id + 1U};
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), identity);
    ASSERT_TRUE(
        writer.add_record({ayni::durability::snapshot_commit_record_type,
                           ayni::durability::snapshot_commit_record_version,
                           ayni::durability::record_required, static_cast<uint32_t>(body.size())},
                          body.data()));
    bytes.resize(writer.finish());
    image.log.push_back({identity.commit_seq, std::move(bytes)});
}

TEST(OwnerBindingSnapshot, paged_round_trip_keeps_the_original_binding_stamp_and_expected_owner) {
    WorkNode node;
    node.run();
    ASSERT_NE(KernelTestProbe::work(node.kernel()).principals.binding(), nullptr);
    const auto binding = *KernelTestProbe::work(node.kernel()).principals.binding();
    ASSERT_EQ(node.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    node.node().recover(); // Store the pause without running application work.
    const Snapshot snapshot = snapshot_of(node);
    ASSERT_GT(snapshot.pages, 1U);
    ASSERT_GE(snapshot.bytes.size(), binding_bytes);
    const auto first = ayni::durability::read_record_header(snapshot.bytes.data());
    EXPECT_EQ(first.type, ayni::authority::owner_binding_record_type);
    EXPECT_EQ(first.version, ayni::authority::owner_binding_record_version);
    EXPECT_EQ(first.flags, ayni::durability::record_required);
    EXPECT_EQ(first.length, ayni::authority::owner_binding_record_size + snapshot_stamp_size);
    ASSERT_LT(binding.stamp.commit_seq, snapshot.metadata.covers_commit_seq);

    KernelTestProbe::replay(node.kernel()).discard_for_reload();
    auto &principals = KernelTestProbe::work(node.kernel()).principals;
    EXPECT_FALSE(principals.binding_known());
    ASSERT_NE(principals.owner(), nullptr);
    EXPECT_EQ(principals.owner()->principal_id, owner_id);
    EXPECT_EQ(principals.owner()->binding_revision, owner_binding);
    ASSERT_EQ(import_snapshot(KernelTestProbe::work(node.kernel()), snapshot), BatchCheck::ok);
    ASSERT_NE(principals.binding(), nullptr);
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_EQ(principals.binding()->principal_id, binding.principal_id);
    EXPECT_EQ(principals.binding()->binding_revision, binding.binding_revision);
    EXPECT_EQ(principals.binding()->stamp.transition_id, binding.stamp.transition_id);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, binding.stamp.commit_seq);
}

TEST(OwnerBindingSnapshot, duplicate_or_nonleading_binding_is_corrupt) {
    WorkNode source;
    source.run();
    const Snapshot original = snapshot_of(source);
    ASSERT_GE(original.bytes.size(), binding_bytes);
    for (const bool duplicate : {true, false}) {
        SCOPED_TRACE(duplicate ? "duplicate" : "after instances");
        Snapshot changed = original;
        if (duplicate) {
            changed.bytes.insert(changed.bytes.begin(), original.bytes.begin(),
                                 original.bytes.begin() + binding_bytes);
            ++changed.metadata.record_count;
            changed.metadata.payload_length += binding_bytes;
        } else {
            changed.bytes.erase(changed.bytes.begin(), changed.bytes.begin() + binding_bytes);
            changed.bytes.insert(changed.bytes.end(), original.bytes.begin(),
                                 original.bytes.begin() + binding_bytes);
        }
        WorkNode restored;
        restored.run();
        KernelTestProbe::replay(restored.kernel()).discard_for_reload();
        EXPECT_EQ(import_snapshot(KernelTestProbe::work(restored.kernel()), changed),
                  BatchCheck::corrupt);
        KernelTestProbe::replay(restored.kernel()).discard_for_reload();
    }
}

TEST(OwnerBindingSnapshot, an_owner_binding_cannot_be_marked_optional) {
    WorkNode source;
    source.run();
    const Snapshot original = snapshot_of(source);
    ASSERT_GE(original.bytes.size(), binding_bytes);
    for (const uint16_t version : {uint16_t{1}, uint16_t{2}}) {
        SCOPED_TRACE(version);
        Snapshot changed = original;
        auto header = ayni::durability::read_record_header(changed.bytes.data());
        ASSERT_EQ(header.type, ayni::authority::owner_binding_record_type);
        header.flags = 0;
        header.version = version;
        ayni::durability::write_record_header(header, changed.bytes.data());
        WorkNode restored;
        restored.run();
        KernelTestProbe::replay(restored.kernel()).discard_for_reload();
        EXPECT_EQ(import_snapshot(KernelTestProbe::work(restored.kernel()), changed),
                  BatchCheck::corrupt);
        EXPECT_FALSE(KernelTestProbe::work(restored.kernel()).principals.binding_known());
    }
}

TEST(OwnerBindingSnapshot, binding_stamp_must_be_nonzero_and_within_the_snapshot_boundary) {
    WorkNode source;
    source.run();
    const Snapshot original = snapshot_of(source);
    ASSERT_GE(original.bytes.size(), binding_bytes);
    for (const bool transition : {true, false}) {
        for (const bool zero : {true, false}) {
            SCOPED_TRACE(transition ? "transition" : "commit");
            SCOPED_TRACE(zero ? "zero" : "beyond boundary");
            Snapshot changed = original;
            const std::size_t offset = record_prefix + ayni::authority::owner_binding_record_size +
                                       (transition ? 0U : snapshot_commit_offset);
            const uint64_t boundary = transition ? original.metadata.max_transition_id
                                                 : original.metadata.covers_commit_seq;
            ayni::foundation::store_le64(changed.bytes.data() + offset, zero ? 0 : boundary + 1U);
            WorkNode restored;
            restored.run();
            KernelTestProbe::replay(restored.kernel()).discard_for_reload();
            EXPECT_EQ(import_snapshot(KernelTestProbe::work(restored.kernel()), changed),
                      BatchCheck::corrupt);
            EXPECT_FALSE(KernelTestProbe::work(restored.kernel()).principals.binding_known());
        }
    }
}

// The reader validates stored facts. Boot separately compares them with
// the Host's expectation and classifies a mismatch as an ownership fault.
TEST(OwnerBindingSnapshot, a_host_binding_mismatch_is_not_snapshot_corruption) {
    WorkNode source;
    source.run();
    const Snapshot snapshot = snapshot_of(source);
    Node restored;
    restored.on_create([](ayni_kernel_t *kernel) {
        auto owner = owner_desc();
        ++owner.binding_revision;
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
    });
    restored.create();
    auto &model = KernelTestProbe::work(restored.kernel());
    ASSERT_EQ(import_snapshot(model, snapshot), BatchCheck::ok);
    ASSERT_TRUE(model.principals.binding_known());
    EXPECT_FALSE(model.principals.binding_matches());
    EXPECT_EQ(model.principals.binding()->binding_revision, owner_binding);
    EXPECT_EQ(model.principals.owner()->binding_revision, owner_binding + 1U);
}

TEST(OwnerBindingSnapshot,
     an_unbound_snapshot_preserves_diagnostics_but_cannot_acquire_a_new_owner) {
    WorkNode node;
    node.run();
    const auto body = WorkNode::create_body(node.instance(agent_a), 0, "timer", 1, {nullptr, 0},
                                            AYNI_TRIGGER_ONCE);
    const auto created = node.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    node.node().step(1); // Derive the task; do not let its strategy decide it.
    node.node().recover();
    auto &model = KernelTestProbe::work(node.kernel());
    ASSERT_EQ(model.goals.size(), 1U);
    ASSERT_EQ(model.tasks.size(), 1U);
    const auto goal = model.goals.at(0);
    const auto task = model.tasks.at(0);
    ASSERT_EQ(task.lifecycle, AYNI_TASK_READY);
    model.principals.drop_restored_binding();
    const Snapshot snapshot = snapshot_of(node, 256);
    ASSERT_GE(snapshot.bytes.size(), record_prefix);
    EXPECT_EQ(ayni::durability::read_record_header(snapshot.bytes.data()).type,
              ayni::work::instance_record_type);
    KernelTestProbe::replay(node.kernel()).discard_for_reload();
    ASSERT_EQ(import_snapshot(model, snapshot), BatchCheck::ok);
    EXPECT_FALSE(model.principals.binding_known());
    ASSERT_EQ(model.goals.size(), 1U);
    ASSERT_EQ(model.tasks.size(), 1U);
    EXPECT_EQ(model.goals.at(0).goal_id, goal.goal_id);
    EXPECT_EQ(model.goals.at(0).commit_seq, goal.commit_seq);
    EXPECT_EQ(model.tasks.at(0).task_id, task.task_id);
    EXPECT_EQ(model.tasks.at(0).commit_seq, task.commit_seq);
    EXPECT_EQ(model.goals.at(0).hold_reason, AYNI_REASON_NONE);
    EXPECT_FALSE(ayni::work::TaskTable::is_held(model.tasks.at(0)));
    // White-box: raw import retains diagnostic facts; the boot must refuse
    // this image before finish/startup can acquire any new owner (ADR-0038).
    EXPECT_FALSE(KernelTestProbe::replay(node.kernel()).binding_matches());
    EXPECT_EQ(model.goals.at(0).commit_seq, goal.commit_seq);
    EXPECT_EQ(model.tasks.at(0).commit_seq, task.commit_seq);
}

TEST(OwnerBindingReplay, the_binding_is_restored_once_without_replacing_the_expected_owner) {
    WorkNode node;
    node.run();
    ASSERT_NE(KernelTestProbe::work(node.kernel()).principals.binding(), nullptr);
    const auto binding = *KernelTestProbe::work(node.kernel()).principals.binding();
    std::array<uint8_t, ayni::authority::owner_binding_record_size> body{};
    ayni::authority::write_owner_binding_record(binding, body.data());
    const ayni::durability::RecordHeader header{
        ayni::authority::owner_binding_record_type, ayni::authority::owner_binding_record_version,
        ayni::durability::record_required, static_cast<uint32_t>(body.size())};
    const ayni::durability::BatchIdentity batch{1, binding.stamp.commit_seq,
                                                binding.stamp.transition_id};
    const uint64_t boot_epoch = KernelTestProbe::boot(node.kernel()).boot_epoch();
    auto &replay = KernelTestProbe::replay(node.kernel());
    auto &principals = KernelTestProbe::work(node.kernel()).principals;
    replay.discard_for_reload();
    for (const uint16_t version : {uint16_t{1}, uint16_t{2}}) {
        auto optional = header;
        optional.flags = 0;
        optional.version = version;
        EXPECT_EQ(ayni::kernel::WorkReplay::classify_header(optional), RecordUse::corrupt);
        EXPECT_EQ(
            ayni::kernel::WorkReplay::on_record(&replay, batch, optional, body.data(), boot_epoch),
            RecordUse::corrupt);
        EXPECT_FALSE(principals.binding_known());
    }
    ASSERT_EQ(ayni::kernel::WorkReplay::classify_header(header), RecordUse::applied);
    EXPECT_EQ(ayni::kernel::WorkReplay::on_record(&replay, batch, header, body.data(), boot_epoch),
              RecordUse::applied);
    ASSERT_NE(principals.binding(), nullptr);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, binding.stamp.commit_seq);
    EXPECT_EQ(ayni::kernel::WorkReplay::on_record(&replay, batch, header, body.data(), boot_epoch),
              RecordUse::corrupt);
    replay.discard();
    EXPECT_FALSE(principals.binding_known());
    ASSERT_NE(principals.owner(), nullptr);
    EXPECT_EQ(principals.owner()->principal_id, owner_id);
    EXPECT_EQ(principals.owner()->binding_revision, owner_binding);
}

TEST(OwnerBindingJournal, the_binding_and_initial_instances_share_one_transition_and_batch) {
    WorkNode node;
    node.run();
    ASSERT_NE(KernelTestProbe::work(node.kernel()).principals.binding(), nullptr);
    const auto binding = *KernelTestProbe::work(node.kernel()).principals.binding();
    uint32_t bindings = 0;
    for (const auto &stored : node.node().sim().log()) {
        ayni::durability::BatchView batch{};
        ASSERT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                  BatchCheck::ok);
        ayni::durability::RecordReader reader(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        uint32_t instances = 0;
        bool bound = false;
        for (uint32_t index = 0; index < batch.record_count; ++index) {
            ASSERT_EQ(reader.next(header, body), ayni::durability::RecordReader::Next::record);
            if (header.type == ayni::authority::owner_binding_record_type) {
                ++bindings;
                bound = true;
                EXPECT_EQ(index, 0U);
                EXPECT_EQ(header.flags, ayni::durability::record_required);
                EXPECT_EQ(batch.identity.commit_seq, binding.stamp.commit_seq);
                EXPECT_EQ(batch.identity.transition_id, binding.stamp.transition_id);
                ayni::authority::OwnerBindingRecord record{};
                ASSERT_TRUE(
                    ayni::authority::read_owner_binding_record(body, header.length, record));
                EXPECT_EQ(record.principal_id, owner_id);
                EXPECT_EQ(record.binding_revision, owner_binding);
            } else if (header.type == ayni::work::instance_record_type) {
                ++instances;
                ayni::work::InstanceRecord record{};
                ASSERT_TRUE(ayni::work::read_instance_record(body, header.length, record));
                ASSERT_NE(
                    KernelTestProbe::work(node.kernel()).agents.find_instance(record.instance_id),
                    nullptr);
                const auto &instance =
                    *KernelTestProbe::work(node.kernel()).agents.find_instance(record.instance_id);
                EXPECT_EQ(instance.stamp.commit_seq, batch.identity.commit_seq);
                EXPECT_EQ(instance.stamp.transition_id, batch.identity.transition_id);
            }
        }
        EXPECT_EQ(reader.next(header, body), ayni::durability::RecordReader::Next::end);
        if (bound) {
            EXPECT_EQ(instances, 2U);
        }
    }
    EXPECT_EQ(bindings, 1U);
}

// Re-encode the frame so its CRC remains valid: recovery must reject the
// missing ownership flag, rather than merely detect damaged bytes.
TEST(OwnerBindingJournal, an_optional_binding_is_rejected_before_recovery_writes) {
    WorkNode source;
    source.run();
    const Snapshot snapshot = snapshot_of(source);
    for (const bool covered : {false, true}) {
        SCOPED_TRACE(covered ? "covered history" : "uncovered log");
        auto original = source.node().sim().durable_storage();
        if (covered) {
            activate_snapshot(snapshot, original);
            WorkNode valid;
            valid.node().sim().durable_storage() = original;
            valid.run();
            ASSERT_EQ(KernelTestProbe::boot(valid.kernel()).generation(),
                      snapshot.metadata.generation);
        }
        for (const uint16_t version : {uint16_t{1}, uint16_t{2}}) {
            SCOPED_TRACE(version);
            auto image = original;
            uint32_t changed = 0;
            for (auto &stored : image.log) {
                ayni::durability::BatchView batch{};
                ASSERT_EQ(
                    ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                    BatchCheck::ok);
                std::vector<uint8_t> bytes(stored.bytes.size());
                ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity,
                                                     batch.schema);
                ayni::durability::RecordReader reader(batch);
                ayni::durability::RecordHeader header{};
                const uint8_t *body = nullptr;
                for (uint32_t index = 0; index < batch.record_count; ++index) {
                    ASSERT_EQ(reader.next(header, body),
                              ayni::durability::RecordReader::Next::record);
                    if (header.type == ayni::authority::owner_binding_record_type) {
                        ++changed;
                        if (covered) {
                            EXPECT_LE(batch.identity.commit_seq,
                                      snapshot.metadata.covers_commit_seq);
                        }
                        header.flags = 0;
                        header.version = version;
                    }
                    ASSERT_TRUE(writer.add_record(header, body));
                }
                bytes.resize(writer.finish());
                stored.bytes = std::move(bytes);
            }
            ASSERT_EQ(changed, 1U);
            WorkNode restored;
            restored.node().sim().durable_storage() = image;
            restored.node().create_and_start();
            restored.node().recover();
            EXPECT_EQ(restored.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
            EXPECT_EQ(restored.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
            ASSERT_EQ(restored.node().sim().log().size(), image.log.size());
            for (std::size_t index = 0; index < image.log.size(); ++index) {
                EXPECT_EQ(restored.node().sim().log()[index].bytes, image.log[index].bytes);
            }
        }
    }
}

} // namespace
