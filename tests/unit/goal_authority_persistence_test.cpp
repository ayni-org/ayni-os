// ADR-0037 decisions 7–10: persistence fault fixtures attach validated facts
// explicitly (White-box), then exercise the real journal,
// snapshot, boot and ordinary Grant/Goal APIs. No fixture bypasses the reader.
#include "authority/grant/grant_records.h"
#include "authority/scope/scope.h"
#include "durability/batch/batch_codec.h"
#include "goal_approval_request.h"
#include "kernel/command/command_records.h"
#include "kernel/work_replay.h"
#include "kernel/work_snapshot.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"
#include "work_node.h"

#include <ayni/authority.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using namespace ayni::test;
using namespace ayni::durability;
using namespace ayni::work;
using ayni::kernel::WorkReplay;

struct Record {
    RecordHeader header;
    std::vector<uint8_t> body;
};

struct Batch {
    BatchIdentity identity;
    std::vector<Record> records;
};

Batch decode(const ayni::hosts::sim::StoredBatch &stored) {
    BatchView view{};
    EXPECT_EQ(read_batch(stored.bytes.data(), stored.bytes.size(), view), BatchCheck::ok);
    Batch batch{view.identity, {}};
    RecordReader reader(view);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    while (reader.next(header, body) == RecordReader::Next::record) {
        batch.records.push_back({header, {body, body + header.length}});
    }
    return batch;
}

ayni::hosts::sim::StoredBatch encode(const Batch &batch) {
    std::size_t body_bytes = 0;
    for (const auto &record : batch.records) {
        body_bytes += record.body.size();
    }
    std::vector<uint8_t> bytes(batch_size(body_bytes, batch.records.size()));
    BatchWriter writer(bytes.data(), bytes.size(), batch.identity);
    for (const auto &record : batch.records) {
        EXPECT_TRUE(writer.add_record(record.header, record.body.data()));
    }
    EXPECT_EQ(writer.finish(), bytes.size());
    return {batch.identity.commit_seq, std::move(bytes)};
}

class Fixture {
  public:
    uint32_t proposal_count = 1;
    bool narrow_first = false;
    Fixture() {
        work.node().config().grant_capacity = 1;
        work.node().config().command_capacity = 8;
        work.node().config().goal_type_capacity = 4;
    }
    void run() {
        ayni_authority_value_set_v1 domain{};
        domain.struct_size = sizeof(domain);
        domain.kind = AYNI_AUTHORITY_I64;
        domain.unit = text("count");
        domain.values.integer = {0, 100};
        const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                 domain};
        start_with_fields(&field, 1);
    }
    void run_with_named_targets(uint32_t field_count = 2) {
        std::array<std::string, AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX> names;
        std::array<ayni_string_v1, AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX> members{};
        for (uint32_t index = 0; index < members.size(); ++index) {
            names[index] = std::string(AYNI_CATALOG_ID_MAX_BYTES - 1U, 'x') + std::to_string(index);
            members[index] = text(names[index].c_str());
        }
        ayni_authority_value_set_v1 domain{};
        domain.struct_size = sizeof(domain);
        domain.kind = AYNI_AUTHORITY_IDENTIFIER;
        domain.values.tokens.member_count = static_cast<uint32_t>(members.size());
        domain.values.tokens.members = members.data();
        std::array<ayni_authority_field_desc_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
        for (uint32_t index = 0; index < field_count; ++index) {
            fields[index] = {sizeof(fields[index]), index + 1U, AYNI_AUTHORITY_PARAMETER, 0,
                             domain};
        }
        start_with_fields(fields.data(), field_count);
    }
    void start_with_fields(const ayni_authority_field_desc_v1 *fields, uint32_t count) {
        work.node().create();
        ASSERT_NE(work.kernel(), nullptr);
        const ayni_capability_desc_v1 capability{
            sizeof(capability), 1, text("device.move"), count, 0, fields};
        ASSERT_EQ(ayni_register_capability(work.kernel(), &capability), AYNI_OK);
        auto proposal = goal_type_desc("proposal", 1, nullptr, this);
        proposal.export_requirements = [](void *ctx, ayni_bytes_v1,
                                          const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_scope_v1 full{sizeof(full), 0, nullptr};
            ayni_authority_value_set_v1 values{};
            values.struct_size = sizeof(values);
            values.kind = AYNI_AUTHORITY_I64;
            values.unit = text("count");
            values.values.integer = {0, 10};
            const ayni_scope_field_v1 field{sizeof(field), 1, values};
            const ayni_scope_v1 narrow{sizeof(narrow), 1, &field};
            for (uint32_t index = 0; index < static_cast<Fixture *>(ctx)->proposal_count; ++index) {
                const ayni_goal_requirement_v1 need{
                    sizeof(need), 1, text("device.move"),
                    index == 0U && static_cast<Fixture *>(ctx)->narrow_first ? narrow : full};
                const auto status = sink->emit(sink->ctx, &need);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            return AYNI_OK;
        };
        ASSERT_EQ(ayni_register_goal_type(work.kernel(), &proposal), AYNI_OK);
        ASSERT_EQ(ayni_start(work.kernel()), AYNI_OK);
        work.node().recover();
        if (work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING) {
            work.reopen_session();
        }
    }
    Submitted create_grant() {
        const ayni_scope_v1 full{sizeof(ayni_scope_v1), 0, nullptr};
        return create_grant_for(work.instance(agent_a), full);
    }
    Submitted create_grant_for(uint64_t subject, const ayni_scope_v1 &scope) {
        const ayni_grant_create_v1 body{
            sizeof(body), 1,    text("device.move"), subject, 0, AYNI_GRANT_CURRENT_BOOT, 0,
            duration_us,  scope};
        auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &body);
        intent.command_id = next_command_++;
        return work.submit(intent);
    }
    uint64_t goal() {
        const auto created =
            work.create(WorkNode::create_body(work.instance(agent_a), 0, "folder"));
        EXPECT_EQ(created.status, AYNI_OK);
        work.node().recover();
        return created.result.object.id;
    }
    uint32_t goal_index(uint64_t id) {
        uint32_t index = 0;
        EXPECT_TRUE(work.kernel()->work.goals.find(id, index));
        return index;
    }
    GoalAuthorityRecord metadata(uint64_t goal_id, uint64_t grant_id = 0, uint32_t count = 1) {
        auto &model = work.kernel()->work;
        const auto &goal = model.goals.at(goal_index(goal_id));
        GoalAuthorityRecord record{};
        record.goal_id = goal_id;
        record.stamp = {goal.created_transition_id, goal.commit_seq};
        record.binding = model.goal_authority.at(goal_index(goal_id)).binding;
        if (grant_id != 0U) {
            uint32_t index = 0;
            EXPECT_TRUE(model.grants.find(grant_id, index));
            const auto &grant = model.grants.at(index);
            record.count = count;
            for (uint32_t requirement = 0; requirement < count; ++requirement) {
                record.requirements[requirement] = {grant.capability_id,
                                                    grant.capability_version,
                                                    model.grants.source_domain(index),
                                                    model.grants.scope(index),
                                                    {grant_id, 1}};
            }
        }
        return record;
    }
    void attach(uint64_t goal_id, uint64_t grant_id = 0, uint32_t count = 1) {
        const auto record = metadata(goal_id, grant_id, count);
        ASSERT_TRUE(valid_goal_authority_record(record));
        ASSERT_EQ(work.kernel()->work.goal_authority.install_validated(goal_index(goal_id), record),
                  AYNI_OK);
        // White-box: replace the durable creation with the same transition
        // encoded by the real producer, retaining the exact injected facts.
        std::vector<uint8_t> bytes(
            ayni::kernel::transition_batch_max(capacities_of(work.node().config())));
        BatchWriter writer(bytes.data(), bytes.size(),
                           {work.kernel()->log.writer_epoch(), record.stamp.commit_seq,
                            record.stamp.transition_id});
        bytes.resize(ayni::kernel::encode_transition(work.kernel()->work, record.stamp, writer));
        ASSERT_FALSE(bytes.empty());
        auto &log = work.node().sim().log();
        const auto found = std::find_if(log.begin(), log.end(), [&](const auto &batch) {
            return batch.commit_seq == record.stamp.commit_seq;
        });
        ASSERT_NE(found, log.end());
        found->bytes = std::move(bytes);
    }
    void expire() {
        work.node().sim().advance_us(duration_us);
        work.node().step(1);
        work.node().recover();
        ASSERT_EQ(work.kernel()->work.grants.at(0).lifecycle, AYNI_GRANT_EXPIRED);
    }
    GoalAuthorityRecord saved(uint64_t id) {
        GoalAuthorityRecord result{};
        EXPECT_TRUE(work.kernel()->work.goal_authority.read(goal_index(id), id, result));
        return result;
    }
    WorkNode work;
    static constexpr uint64_t duration_us = 1000000;

  private:
    uint64_t next_command_ = 1;
};

struct Snapshot {
    SnapshotMetadata metadata{};
    std::vector<Record> records;
    bool paired_across_page = false;
};

Snapshot snapshot(Fixture &fixture, uint32_t page_size = 512) {
    auto &kernel = *fixture.work.kernel();
    Snapshot result;
    const auto layout = ayni::kernel::measure_snapshot(kernel.work);
    result.metadata.generation = 1;
    result.metadata.covers_commit_seq = kernel.work.transitions.latest().commit_seq;
    result.metadata.max_transition_id = kernel.work.transitions.latest().transition_id;
    result.metadata.boot_epoch = kernel.log.boot_epoch();
    result.metadata.writer_epoch = kernel.log.writer_epoch();
    result.metadata.payload_length = layout.payload_length;
    result.metadata.record_count = layout.record_count;
    ayni::kernel::WorkSnapshotWriter writer(kernel.work);
    std::vector<uint8_t> page(page_size);
    uint16_t last_type = 0;
    for (unsigned round = 0; round < 100; ++round) {
        uint32_t size = 0;
        const auto next = writer.next(page.data(), page_size, size);
        EXPECT_NE(next, ayni::kernel::WorkSnapshotWriter::Page::no_room);
        if (next != ayni::kernel::WorkSnapshotWriter::Page::page) {
            EXPECT_EQ(result.records.size(), layout.record_count);
            return result;
        }
        uint32_t offset = 0;
        while (offset < size) {
            const auto header = read_record_header(page.data() + offset);
            if (offset == 0U && last_type == goal_record_type &&
                header.type == goal_authority_record_type) {
                result.paired_across_page = true;
            }
            offset += static_cast<uint32_t>(record_header_size);
            result.records.push_back(
                {header, {page.data() + offset, page.data() + offset + header.length}});
            offset += header.length;
            last_type = header.type;
        }
    }
    ADD_FAILURE() << "snapshot did not finish";
    return result;
}

BatchCheck import_snapshot(Fixture &fixture, const Snapshot &snapshot) {
    fixture.work.kernel()->replay.discard_for_reload();
    ayni::kernel::WorkSnapshotReader reader(fixture.work.kernel()->work, snapshot.metadata);
    for (const auto &record : snapshot.records) {
        const auto result = reader.apply(record.header, record.body.data());
        if (result != BatchCheck::ok) {
            return result;
        }
    }
    return reader.complete();
}

Record &authority_record(std::vector<Record> &records) {
    const auto found = std::find_if(records.begin(), records.end(), [](const auto &record) {
        return record.header.type == goal_authority_record_type;
    });
    EXPECT_NE(found, records.end());
    return *found;
}

// ADR-0038 decision 13: schema 2 retains explicit empty evidence, while
// missing evidence is corruption even if inner Goal envelopes look historical.
TEST(GoalAuthorityPersistence, journal_round_trip_preserves_zero_and_rejects_missing_evidence) {
    Fixture source;
    source.run();
    const auto explicit_zero = source.goal();
    source.attach(explicit_zero);
    Fixture restored;
    restored.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    restored.run();
    ASSERT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(restored.saved(explicit_zero).count, 0U);
    EXPECT_EQ(restored.work.goal(explicit_zero).hold_reason, AYNI_REASON_LEGACY_UNCONFIRMED);

    // White-box CRC-valid corruption: remove the evidence and downgrade the
    // inner envelopes. This does not make a schema 2 batch a supported legacy image.
    auto missing = decode(source.work.node().sim().durable_storage().log.back());
    missing.records.erase(std::remove_if(missing.records.begin(), missing.records.end(),
                                         [](auto &entry) {
                                             if (entry.header.type == goal_record_type ||
                                                 entry.header.type == goal_state_record_type) {
                                                 entry.header.version = goal_record_version;
                                             }
                                             return entry.header.type == goal_authority_record_type;
                                         }),
                          missing.records.end());
    source.work.node().sim().durable_storage().log.back() = encode(missing);
    Fixture rejected;
    rejected.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    rejected.run();
    EXPECT_EQ(rejected.work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    EXPECT_EQ(rejected.work.kernel()->work.goals.size(), 0U);
}

// C-STORE-015, ADR-0037 decision 8: a version promise is not satisfied by a
// state record alone, a duplicate metadata record or an unmatched later batch.
TEST(GoalAuthorityPersistence, missing_duplicate_or_wrong_batch_metadata_restricts_boot) {
    Fixture source;
    source.run();
    const auto goal = source.goal();
    source.attach(goal);
    const auto image = source.work.node().sim().durable_storage();
    for (unsigned fault = 0; fault < 5; ++fault) {
        SCOPED_TRACE(fault);
        Fixture restored;
        auto changed = image;
        auto batch = decode(changed.log.back());
        auto &record = authority_record(batch.records);
        if (fault == 0U) {
            batch.records.erase(std::remove_if(batch.records.begin(), batch.records.end(),
                                               [](const auto &entry) {
                                                   return entry.header.type ==
                                                          goal_authority_record_type;
                                               }),
                                batch.records.end());
        } else if (fault == 1U) {
            const auto duplicate = record;
            batch.records.push_back(duplicate);
        } else if (fault == 3U) {
            for (auto &entry : batch.records) {
                if (entry.header.type == goal_record_type) {
                    entry.header.version = goal_record_version;
                }
            }
        } else {
            GoalAuthorityRecord decoded{};
            ASSERT_TRUE(
                read_goal_authority_record(record.body.data(), record.header.length, decoded));
            if (fault == 4U) {
                ++decoded.stamp.transition_id;
            } else {
                ++decoded.stamp.commit_seq;
            }
            write_goal_authority_record(decoded, record.body.data());
        }
        changed.log.back() = encode(batch);
        restored.work.node().sim().durable_storage() = changed;
        restored.run();
        EXPECT_EQ(restored.work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
        EXPECT_EQ(restored.work.kernel()->work.goals.size(), 0U);
    }
}

// ADR-0037 decision 8: discovery uses this same classifier for covered history.
TEST(GoalAuthorityPersistence, authority_headers_cannot_be_optional_even_for_future_versions) {
    for (const uint16_t type :
         {goal_record_type, goal_state_record_type, goal_authority_record_type}) {
        for (const uint16_t version : {uint16_t{2}, uint16_t{3}}) {
            EXPECT_EQ(WorkReplay::classify_header({type, version, 0, 0}), RecordUse::corrupt);
        }
    }
    EXPECT_EQ(WorkReplay::classify_header({goal_record_type, goal_record_version, 0, 0}),
              RecordUse::applied);
    EXPECT_EQ(WorkReplay::classify_header(
                  {goal_authority_record_type, goal_authority_record_version, 0, 0}),
              RecordUse::corrupt);
}

// C-STORE-018 / ADR-0037 decision 13: a proposal requires the new Goal
// envelope; downgrading only that envelope must not reinterpret its state.
TEST(GoalAuthorityPersistence, proposed_snapshots_require_the_new_state_envelope) {
    Fixture source;
    source.run();
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    ASSERT_EQ(source.work.goal(created.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
    source.work.node().recover();
    const auto original = snapshot(source);
    for (const uint16_t version : {goal_record_version, goal_with_authority_record_version,
                                   goal_authority_state_record_version}) {
        auto changed = original;
        auto goal =
            std::find_if(changed.records.begin(), changed.records.end(),
                         [](const auto &record) { return record.header.type == goal_record_type; });
        ASSERT_NE(goal, changed.records.end());
        EXPECT_EQ(goal->header.version, goal_authority_state_record_version);
        goal->header.version = version;
        Fixture restored;
        restored.run();
        EXPECT_EQ(import_snapshot(restored, changed), version == goal_authority_state_record_version
                                                          ? BatchCheck::ok
                                                          : BatchCheck::corrupt);
    }
}

// ADR-0037 decision 8: the Goal/metadata promise survives a page boundary.
TEST(GoalAuthorityPersistence, paged_snapshot_keeps_adjacent_metadata_and_original_stamps) {
    Fixture source;
    source.run();
    const auto goal = source.goal();
    source.attach(goal);
    const auto saved = source.saved(goal);
    // White-box record layout: one maximum-size page for this exact GOAL.
    const uint32_t page_size =
        static_cast<uint32_t>(record_header_size) +
        goal_record_size(source.work.kernel()->work.goals.at(source.goal_index(goal))) +
        goal_state_record_size + 16U;
    const auto image = snapshot(source, page_size);
    ASSERT_TRUE(image.paired_across_page);
    Fixture restored;
    restored.run();
    ASSERT_EQ(import_snapshot(restored, image), BatchCheck::ok);
    const auto actual = restored.saved(goal);
    EXPECT_EQ(actual.count, 0U);
    EXPECT_EQ(actual.stamp.transition_id, saved.stamp.transition_id);
    EXPECT_EQ(actual.stamp.commit_seq, saved.stamp.commit_seq);
}

// ADR-0037 decision 8: adjacency, declared count, and independent stamp bounds.
TEST(GoalAuthorityPersistence, snapshots_refuse_orphan_missing_and_future_metadata) {
    Fixture source;
    source.run();
    source.attach(source.goal());
    const auto original = snapshot(source);
    for (unsigned fault = 0; fault < 4; ++fault) {
        SCOPED_TRACE(fault);
        auto changed = original;
        if (fault == 0U) {
            changed.records.erase(std::remove_if(changed.records.begin(), changed.records.end(),
                                                 [](const auto &entry) {
                                                     return entry.header.type ==
                                                            goal_authority_record_type;
                                                 }),
                                  changed.records.end());
            --changed.metadata.record_count;
        } else if (fault == 1U) {
            std::reverse(changed.records.begin(), changed.records.end());
        } else {
            auto &record = authority_record(changed.records);
            GoalAuthorityRecord decoded{};
            ASSERT_TRUE(
                read_goal_authority_record(record.body.data(), record.header.length, decoded));
            if (fault == 2U) {
                decoded.stamp.transition_id = changed.metadata.max_transition_id + 1U;
            } else {
                decoded.stamp.commit_seq = changed.metadata.covers_commit_seq + 1U;
            }
            write_goal_authority_record(decoded, record.body.data());
        }
        Fixture restored;
        restored.run();
        EXPECT_EQ(import_snapshot(restored, changed), BatchCheck::corrupt);
        restored.work.kernel()->replay.discard_for_reload();
    }
}

// ADR-0037 decisions 8/9: live selections preserve Grant identity after expiry;
// a later terminal revision does not make saved selection revision 1 corrupt.
TEST(GoalAuthorityPersistence,
     expired_grant_keeps_live_goal_reference_and_blocks_ordinary_reclaim) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.goal();
    source.attach(goal, grant.result.object.id);
    source.expire();
    const auto before = source.work.kernel()->work.transitions.latest().commit_seq;
    EXPECT_EQ(source.create_grant().status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(source.work.kernel()->work.transitions.latest().commit_seq, before);
    const auto image = snapshot(source);
    Fixture restored;
    restored.run();
    ASSERT_EQ(import_snapshot(restored, image), BatchCheck::ok);
    EXPECT_EQ(restored.saved(goal).requirements[0].dependency.revision, 1U);
    EXPECT_EQ(restored.work.kernel()->work.grants.at(0).revision, 2U);
}

// ADR-0037 decision 9: cancellation is not a durable unlink at API return.
TEST(GoalAuthorityPersistence, a_terminal_goal_releases_its_grant_only_after_writer_confirmation) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.goal();
    source.attach(goal, grant.result.object.id);
    source.expire();
    ASSERT_EQ(source.work.change(AYNI_INTENT_GOAL_CANCEL, goal).status, AYNI_OK);
    EXPECT_EQ(source.create_grant().status, AYNI_CAPACITY_EXCEEDED);
    source.work.node().recover();
    EXPECT_EQ(source.create_grant().status, AYNI_OK);
    // Historical terminal metadata can now name its legally reclaimed Grant.
    EXPECT_TRUE(source.work.kernel()->replay.consistent());
}

// ADR-0037 decision 9: clearing a reused slot must not erase the old release fence.
TEST(GoalAuthorityPersistence, goal_slot_reuse_hands_old_references_to_the_release_fence) {
    Fixture source;
    source.work.node().config().goal_capacity = 1;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.goal();
    source.attach(goal, grant.result.object.id);
    source.expire();
    ASSERT_EQ(source.work.change(AYNI_INTENT_GOAL_CANCEL, goal).status, AYNI_OK);
    source.work.node().recover();
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "folder"));
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_FALSE(source.work.kernel()->work.goal_authority.contains(0, goal));
    EXPECT_TRUE(source.work.kernel()->work.goal_authority.contains(0, created.result.object.id));
    EXPECT_EQ(source.work.kernel()->work.goal_authority.at(0).count, 0U);
    EXPECT_EQ(source.create_grant().status, AYNI_CAPACITY_EXCEEDED);
    source.work.node().recover();
    EXPECT_EQ(source.create_grant().status, AYNI_OK);
}

// ADR-0037 decisions 8/10: smaller configured capacity is not log corruption.
TEST(GoalAuthorityPersistence,
     smaller_requirement_capacity_classifies_journal_and_snapshot_as_no_room) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.attach(source.goal(), grant.result.object.id, 2);
    const auto image = snapshot(source);
    Fixture snapshot_target;
    snapshot_target.work.node().config().goal_requirement_capacity = 1;
    snapshot_target.run();
    EXPECT_EQ(import_snapshot(snapshot_target, image), BatchCheck::no_room);
    snapshot_target.work.kernel()->replay.discard_for_reload();
    Fixture journal_target;
    journal_target.work.node().config().goal_requirement_capacity = 1;
    journal_target.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    journal_target.run();
    EXPECT_EQ(journal_target.work.node().info().restriction,
              AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
}

// ADR-0037 decision 8: metadata bytes alone cannot create a valid authority edge.
TEST(GoalAuthorityPersistence, live_references_to_missing_or_other_schema_grants_are_corrupt) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.attach(source.goal(), grant.result.object.id);
    const auto original = snapshot(source);
    for (unsigned fault = 0; fault < 2; ++fault) {
        auto changed = original;
        auto &record = authority_record(changed.records);
        GoalAuthorityRecord decoded{};
        ASSERT_TRUE(read_goal_authority_record(record.body.data(), record.header.length, decoded));
        if (fault == 0U) {
            ++decoded.requirements[0].dependency.grant_id;
        } else {
            decoded.requirements[0].capability_version += 1U;
        }
        write_goal_authority_record(decoded, record.body.data());
        Fixture target;
        target.run();
        EXPECT_EQ(import_snapshot(target, changed), BatchCheck::corrupt);
        target.work.kernel()->replay.discard_for_reload();
    }
}

// ADR-0037 decision 9: a stale earlier release must never shorten protection.
TEST(GoalAuthorityPersistence,
     release_fences_keep_the_latest_unconfirmed_unlink_and_reject_same_batch_replay) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.expire();
    auto &model = source.work.kernel()->work;
    const auto seq = model.transitions.next_commit_seq();
    model.grants.release_reference(grant.result.object.id, seq + 1U);
    model.grants.release_reference(grant.result.object.id, seq);
    EXPECT_EQ(model.grants.at(0).reference_release_seq, seq + 1U); // White-box fence owner.
    EXPECT_EQ(source.create_grant().status, AYNI_CAPACITY_EXCEEDED);
    ayni::authority::GrantRecord record{};
    record.grant_id = grant.result.object.id + 1U;
    record.replaced_grant_id = grant.result.object.id;
    record.grant = {source.work.instance(agent_a),
                    0,
                    owner_id,
                    owner_binding,
                    model.grants.at(0).capability_id,
                    1,
                    model.grants.source_domain(0),
                    nullptr,
                    AYNI_GRANT_PERMANENT,
                    0,
                    0};
    record.scope = model.grants.scope(0);
    // Rejected before installation, so borrowed input never aliases a write.
    EXPECT_EQ(model.grants.restore_create(
                  record, {model.transitions.latest().transition_id + 1U, seq + 1U}),
              ayni::authority::GrantTable::Restore::corrupt);
}

// ADR-0037 decision 8: a legal maximum requirement count with large canonical
// domains must fit the admitted buffer, even when no other batch records use
// their conservative reserves. This exercises the real writer, not a formula.
TEST(GoalAuthorityPersistence, largest_requirement_count_and_large_regions_fit_the_reserved_batch) {
    Fixture source;
    source.work.node().config().authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX;
    source.work.node().config().goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    source.work.node().config().storage_buffer_bytes = 0;
    source.run_with_named_targets();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.goal();
    source.attach(goal, grant.result.object.id, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);
    ASSERT_FALSE(::testing::Test::HasFailure());
    const auto batch = decode(source.work.node().sim().log().back());
    EXPECT_EQ(batch.records.back().header.type, goal_authority_record_type);
}

// ADR-0037 decision 8: one transition changes only one existing Goal authority
// set. Two independently valid records still cannot consume one record's budget.
TEST(GoalAuthorityPersistence,
     a_second_existing_goal_authority_update_in_the_same_batch_is_corrupt) {
    Fixture source;
    source.run();
    const auto first = source.goal();
    source.attach(first);
    const auto second = source.goal();
    source.attach(second);
    auto &model = source.work.kernel()->work;
    const auto stamp = model.transitions.next();
    const BatchIdentity batch{1, stamp.commit_seq, stamp.transition_id};
    WorkReplay replay(model);
    for (const auto id : {first, second}) {
        // White-box: a valid same-batch ordinary pause precedes each new metadata set.
        auto goal = model.goals.at(source.goal_index(id));
        goal.lifecycle = AYNI_GOAL_PAUSED;
        ++goal.revision;
        std::array<uint8_t, goal_state_record_size> body{};
        write_goal_state_record(goal, body.data());
        ASSERT_EQ(WorkReplay::on_record(&replay, batch,
                                        {goal_state_record_type, goal_state_record_version,
                                         record_required, body.size()},
                                        body.data()),
                  RecordUse::applied);
    }
    for (const auto id : {first, second}) {
        auto record = source.metadata(id);
        record.stamp = stamp;
        std::vector<uint8_t> bytes(goal_authority_record_size(record));
        write_goal_authority_record(record, bytes.data());
        EXPECT_EQ(WorkReplay::on_record(&replay, batch,
                                        {goal_authority_record_type, goal_authority_record_version,
                                         record_required, static_cast<uint32_t>(bytes.size())},
                                        bytes.data()),
                  id == first ? RecordUse::applied : RecordUse::corrupt);
    }
    source.work.kernel()->replay.discard_for_reload();
}

// ADR-0037 decisions 3/8: a restored update can change selected references,
// never the original ceiling or the one-way completion of a proposal.
TEST(GoalAuthorityPersistence,
     restored_updates_keep_the_original_ceiling_and_never_return_to_proposed) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.goal();
    source.attach(goal, grant.result.object.id);
    const auto original = source.metadata(goal, grant.result.object.id);
    auto &table = source.work.kernel()->work.goal_authority;
    for (unsigned change = 0; change < 4; ++change) {
        SCOPED_TRACE(change);
        auto next = original;
        ++next.stamp.commit_seq;
        ++next.stamp.transition_id;
        if (change == 0U) {
            ++next.requirements[0].capability_version;
        } else if (change == 1U) {
            next.phase = GoalAuthorityPhase::proposed;
            next.binding = {};
            next.requirements[0].dependency = {};
        } else if (change == 2U) {
            next.stamp.commit_seq = original.stamp.commit_seq;
        } else {
            next.stamp.transition_id = original.stamp.transition_id;
        }
        ASSERT_TRUE(valid_goal_authority_record(next));
        EXPECT_EQ(table.restore_validated(source.goal_index(goal), next), AYNI_INVALID_ARGUMENT);
        const auto kept = source.saved(goal);
        EXPECT_EQ(kept.stamp.commit_seq, original.stamp.commit_seq);
        EXPECT_EQ(kept.phase, original.phase);
        EXPECT_EQ(kept.requirements[0].capability_version,
                  original.requirements[0].capability_version);
    }
}

// ADR-0037 decision 8: snapshot boundary validity cannot substitute for the
// owning Goal's narrower stamp boundary or the adjacent partner's identity.
TEST(GoalAuthorityPersistence,
     snapshot_metadata_cannot_borrow_a_sibling_or_outlive_its_goal_stamp) {
    Fixture source;
    source.run();
    const auto first = source.goal();
    source.attach(first);
    const auto second = source.goal();
    source.attach(second);
    const auto image = snapshot(source);
    for (unsigned fault = 0; fault < 2; ++fault) {
        auto changed = image;
        std::vector<Record *> metadata;
        for (auto &record : changed.records) {
            if (record.header.type == goal_authority_record_type) {
                metadata.push_back(&record);
            }
        }
        ASSERT_EQ(metadata.size(), 2U);
        if (fault == 0U) {
            auto decoded = source.saved(first);
            decoded.stamp.commit_seq = source.saved(second).stamp.commit_seq;
            write_goal_authority_record(decoded, metadata[0]->body.data());
        } else {
            // Make the earlier Goal legacy, so the wrong sibling has no existing
            // table entry whose monotone stamp check could hide the pairing bug.
            for (auto &record : changed.records) {
                if (record.header.type == goal_record_type) {
                    record.header.version = goal_record_version;
                    break;
                }
            }
            auto decoded = source.saved(first);
            write_goal_authority_record(decoded, metadata[1]->body.data());
            changed.records.erase(std::find_if(
                changed.records.begin(), changed.records.end(), [](const auto &record) {
                    return record.header.type == goal_authority_record_type;
                }));
            --changed.metadata.record_count;
        }
        Fixture target;
        target.run();
        EXPECT_EQ(import_snapshot(target, changed), BatchCheck::corrupt);
        target.work.kernel()->replay.discard_for_reload();
    }
}

// C-STORE-018 / ADR-0037 decision 8: a well-formed saved reference cannot transfer
// a Grant from the other Agent instance, even with the same schema and Scope.
TEST(GoalAuthorityPersistence, a_grant_for_another_agent_cannot_authorize_the_restored_goal) {
    Fixture source;
    source.run();
    const ayni_scope_v1 scope{sizeof(ayni_scope_v1), 0, nullptr};
    const auto grant = source.create_grant_for(source.work.instance(agent_b), scope);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.attach(source.goal(), grant.result.object.id);
    Fixture target;
    target.run();
    EXPECT_EQ(import_snapshot(target, snapshot(source)), BatchCheck::corrupt);
    target.work.kernel()->replay.discard_for_reload();
}

// ADR-0037 decision 8: syntactically valid wider needs are still not covered by
// a narrower Grant; checking only Scope parse success would silently expand it.
TEST(GoalAuthorityPersistence, a_narrow_grant_does_not_cover_a_wider_saved_requirement) {
    Fixture source;
    source.run();
    ayni_authority_value_set_v1 value{};
    value.struct_size = sizeof(value);
    value.kind = AYNI_AUTHORITY_I64;
    value.unit = text("count");
    value.values.integer = {10, 20};
    const ayni_scope_field_v1 field{sizeof(field), 1, value};
    const ayni_scope_v1 narrow{sizeof(ayni_scope_v1), 1, &field};
    const auto grant = source.create_grant_for(source.work.instance(agent_a), narrow);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.attach(source.goal(), grant.result.object.id);
    auto image = snapshot(source);
    auto &record = authority_record(image.records);
    GoalAuthorityRecord decoded{};
    ASSERT_TRUE(read_goal_authority_record(record.body.data(), record.header.length, decoded));
    const ayni_scope_v1 full{sizeof(ayni_scope_v1), 0, nullptr};
    uint32_t size = 0;
    ASSERT_EQ(ayni::authority::normalize_scope(decoded.requirements[0].source_domain, full, nullptr,
                                               0, size),
              AYNI_BUFFER_TOO_SMALL);
    std::vector<uint8_t> bytes(size);
    ASSERT_EQ(ayni::authority::normalize_scope(decoded.requirements[0].source_domain, full,
                                               bytes.data(), size, size),
              AYNI_OK);
    decoded.requirements[0].scope = {bytes.data(), size};
    ASSERT_TRUE(valid_goal_authority_record(decoded));
    ASSERT_EQ(goal_authority_record_size(decoded), record.body.size());
    write_goal_authority_record(decoded, record.body.data());
    Fixture target;
    target.run();
    EXPECT_EQ(import_snapshot(target, image), BatchCheck::corrupt);
    target.work.kernel()->replay.discard_for_reload();
}

// ADR-0037 decisions 9/10: FAILED and both UNKNOWN outcomes choose durable
// history. Discard clears a later in-memory unlink; a reused commit sequence
// cannot release the old live Goal's reconstructed dependency.
TEST(GoalAuthorityPersistence,
     failed_or_unknown_unlinks_reload_the_durable_references_before_reclaim) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    for (const auto fault :
         {StorageFault::fail, StorageFault::unknown_lost, StorageFault::unknown_persisted}) {
        SCOPED_TRACE(static_cast<int>(fault));
        Fixture source;
        source.work.node().config().goal_capacity = 1;
        source.run();
        const auto grant = source.create_grant();
        ASSERT_EQ(grant.status, AYNI_OK);
        source.work.node().recover();
        const auto goal = source.goal();
        source.attach(goal, grant.result.object.id);
        source.expire();
        const auto old_boot = source.work.node().info().boot_epoch;
        source.work.node().sim().inject_fault(StoragePort::append, fault);
        source.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        source.work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
        ASSERT_EQ(source.work.change(AYNI_INTENT_GOAL_CANCEL, goal).status, AYNI_OK);
        const auto replacement =
            source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "folder"));
        ASSERT_EQ(replacement.status, AYNI_OK);
        // White-box precondition: the old row disappeared before its release
        // was confirmed, and the Grant owns that unconfirmed release fence.
        ASSERT_FALSE(source.work.kernel()->work.goal_authority.contains(0, goal));
        ASSERT_EQ(source.work.kernel()->work.grants.at(0).reference_release_seq,
                  replacement.result.commit_seq);
        for (unsigned round = 0; round < 5000; ++round) {
            const auto report = source.work.node().step(1);
            source.work.node().sim().complete_storage();
            if (source.work.node().info().boot_epoch > old_boot &&
                source.work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING &&
                source.work.kernel()->log.idle()) {
                break;
            }
            if (report.more_ready == 0U && source.work.node().sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > source.work.node().sim().now_us()) {
                source.work.node().sim().advance_us(report.next_deadline_us -
                                                    source.work.node().sim().now_us());
            }
        }
        ASSERT_GT(source.work.node().info().boot_epoch, old_boot);
        ASSERT_EQ(source.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        source.work.reopen_session();
        if (fault != StorageFault::unknown_persisted) {
            const auto independent = source.work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_b);
            ASSERT_EQ(independent.status, AYNI_OK);
            // The recovered startup branch already reused the abandoned seq;
            // advancing the current writer beyond it still cannot release a live ref.
            EXPECT_GT(independent.result.commit_seq, replacement.result.commit_seq);
            const auto &log = source.work.node().sim().log();
            EXPECT_NE(std::find_if(log.begin(), log.end(),
                                   [&](const auto &batch) {
                                       return batch.commit_seq == replacement.result.commit_seq;
                                   }),
                      log.end());
            source.work.node().recover();
            EXPECT_EQ(source.saved(goal).requirements[0].dependency.grant_id,
                      grant.result.object.id);
            EXPECT_EQ(source.create_grant().status, AYNI_CAPACITY_EXCEEDED);
        } else {
            EXPECT_EQ(source.work.goal(goal).lifecycle, AYNI_GOAL_CANCELLED);
            EXPECT_EQ(source.create_grant().status, AYNI_OK);
        }
    }
}
// White-box snapshot fault helper: keep the exact Grant body and stamps,
// add only a Goal binding and its required version, updating framing lengths.
void bind_snapshot_grant(Fixture &source, Snapshot &image, uint64_t goal_id) {
    auto grant = source.work.kernel()->work.grants.at(0);
    grant.bound_goal_id = goal_id;
    const auto found = std::find_if(image.records.begin(), image.records.end(), [](const auto &r) {
        return r.header.type == ayni::authority::grant_record_type;
    });
    ASSERT_NE(found, image.records.end());
    const auto old_size = found->header.length;
    found->header.version = ayni::authority::goal_grant_snapshot_record_version;
    found->header.length =
        ayni::authority::grant_snapshot_record_size(grant, found->header.version);
    found->body.resize(found->header.length);
    ayni::authority::write_grant_snapshot_record(
        grant, source.work.kernel()->work.grants.source_domain(0),
        source.work.kernel()->work.grants.scope(0), found->body.data(), found->header.version);
    image.metadata.payload_length += found->header.length - old_size;
}

// C-STORE-019: an ordinary command's complete identity did not approve a Goal binding.
TEST(GoalAuthorityPersistence, ordinary_create_mapping_cannot_prove_a_bound_grant) {
    Fixture source;
    source.run();
    const auto goal = source.goal();
    ASSERT_EQ(source.create_grant().status, AYNI_OK);
    source.work.node().recover();
    auto image = snapshot(source);
    bind_snapshot_grant(source, image, goal);
    Fixture restored;
    restored.run();
    EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
}

// C-STORE-018/019: matching subject, schema and Scope still cannot transfer a binding.
TEST(GoalAuthorityPersistence, snapshot_dependency_cannot_borrow_another_goals_bound_grant) {
    Fixture source;
    source.run();
    const auto owner = source.goal();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    const auto other = source.goal();
    source.attach(other, grant.result.object.id);
    auto image = snapshot(source);
    bind_snapshot_grant(source, image, owner);
    // A legitimate compacted snapshot may no longer retain creation mappings.
    // Remove them to isolate the dependency check from command proof validation.
    image.records.erase(std::remove_if(image.records.begin(), image.records.end(),
                                       [&](const auto &r) {
                                           if (r.header.type != ayni::kernel::command_record_type) {
                                               return false;
                                           }
                                           image.metadata.payload_length -=
                                               record_header_size + r.header.length;
                                           --image.metadata.record_count;
                                           return true;
                                       }),
                        image.records.end());
    Fixture restored;
    restored.run();
    EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
}

// C-AUTH-032/C-STORE-019: actual public approval produces one complete journal
// transition; both boot and snapshot preserve exact binding and retry evidence.
TEST(GoalApprovalPersistence, public_approval_survives_journal_and_snapshot_with_exact_retry) {
    Fixture source;
    source.run();
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), created.result.object.id);
    auto intent =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 100;
    const auto approved = source.work.submit(intent);
    ASSERT_EQ(approved.status, AYNI_OK);
    source.work.node().recover();
    const auto grant_id =
        source.saved(created.result.object.id).requirements[0].dependency.grant_id;
    ASSERT_NE(grant_id, 0U);
    const auto batch = decode(source.work.node().sim().durable_storage().log.back());
    ASSERT_EQ(batch.records.size(), 4U);
    EXPECT_EQ(batch.records.front().header.type, ayni::authority::grant_record_type);
    EXPECT_EQ(batch.records.back().header.version, ayni::kernel::approval_command_record_version);
    Fixture booted;
    booted.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    booted.run();
    ASSERT_EQ(booted.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(booted.saved(created.result.object.id).requirements[0].dependency.grant_id, grant_id);
    EXPECT_EQ(booted.work.kernel()->work.grants.at(0).bound_goal_id, created.result.object.id);
    // E retains activated-work recovery hold; F alone removes it after rechecking.
    EXPECT_EQ(booted.work.goal(created.result.object.id).hold_reason,
              AYNI_REASON_LEGACY_UNCONFIRMED);
    intent.session_id = booted.work.session();
    const auto retry = booted.work.submit(intent);
    ASSERT_EQ(retry.status, AYNI_OK);
    EXPECT_EQ(retry.result.commit_seq, approved.result.commit_seq);
    Fixture compacted;
    compacted.run();
    ASSERT_EQ(import_snapshot(compacted, snapshot(source)), BatchCheck::ok);
    EXPECT_EQ(compacted.saved(created.result.object.id).requirements[0].dependency.grant_id,
              grant_id);
    EXPECT_EQ(compacted.work.kernel()->work.grants.at(0).bound_goal_id, created.result.object.id);
}

// C-STORE-019: removing any partner or changing approval identity/Grant lifetime
// must restrict boot, never recover a partially usable approval.
TEST(GoalApprovalPersistence, missing_partner_or_mismatched_exact_command_restricts_recovery) {
    Fixture source;
    source.run();
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), created.result.object.id);
    auto intent =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 100;
    ASSERT_EQ(source.work.submit(intent).status, AYNI_OK);
    source.work.node().recover();
    const auto image = source.work.node().sim().durable_storage();
    const auto original = decode(image.log.back());
    ASSERT_EQ(original.records.size(), 4U);
    for (uint32_t fault = 0; fault < 6; ++fault) {
        SCOPED_TRACE(fault);
        auto batch = original;
        if (fault < 4U) {
            batch.records.erase(batch.records.begin() + fault);
        } else if (fault == 4U) {
            auto &record = batch.records.front();
            auto grant = source.work.kernel()->work.grants.at(0);
            grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
            grant.duration_us = 10000;
            // White-box: valid differing lifetime through the real writer.
            ayni::authority::write_grant_record(
                grant, source.work.kernel()->work.grants.source_domain(0),
                source.work.kernel()->work.grants.scope(0), record.body.data());
        } else {
            batch.records.push_back(batch.records.front());
        }
        Fixture restored;
        restored.work.node().sim().durable_storage() = image;
        restored.work.node().sim().durable_storage().log.back() = encode(batch);
        restored.run();
        EXPECT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(restored.work.kernel()->work.grants.size(), 0U);
    }
}

// C-STORE-019: command identity remains part of compacted evidence while facts
// are retained; corruption cannot hide behind the absence of journal pairing.
TEST(GoalApprovalPersistence, snapshot_command_echo_cannot_disagree_with_retained_creation) {
    Fixture source;
    source.run();
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), created.result.object.id);
    auto intent =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 100;
    ASSERT_EQ(source.work.submit(intent).status, AYNI_OK);
    source.work.node().recover();
    auto image = snapshot(source);
    for (auto &record : image.records) {
        if (record.header.type != ayni::kernel::command_record_type ||
            record.header.version != ayni::kernel::approval_command_record_version) {
            continue;
        }
        ayni::kernel::CommandMapping mapping{};
        uint64_t floor = 0;
        ayni_bytes_v1 canonical{};
        constexpr uint32_t snapshot_tail = sizeof(uint64_t);
        ASSERT_TRUE(ayni::kernel::read_command_record(record.header.version, record.body.data(),
                                                      record.header.length - snapshot_tail, mapping,
                                                      floor, canonical));
        ayni::kernel::ApprovalCommand echo{};
        ASSERT_TRUE(ayni::kernel::read_approval_command(canonical, echo));
        ++echo.approval.original.expiry_us;
        std::vector<uint8_t> changed(canonical.size);
        uint32_t size = 0;
        ASSERT_EQ(ayni::kernel::encode_approval_command(echo, changed.data(), canonical.size, size),
                  AYNI_OK);
        mapping.digest = ayni::kernel::authority_command_digest({changed.data(), size});
        ASSERT_TRUE(ayni::kernel::write_command_record(mapping, floor, {changed.data(), size},
                                                       record.body.data(),
                                                       record.header.length - snapshot_tail));
    }
    Fixture restored;
    restored.run();
    EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
}

// C-STORE-019: a same-phase metadata update cannot fill just one missing item
// with an unrelated Grant outside the complete approval transition.
TEST(GoalApprovalPersistence, unpaired_metadata_cannot_change_original_dependencies) {
    Fixture source;
    source.proposal_count = 2;
    source.run();
    const auto goal =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    ASSERT_EQ(goal.status, AYNI_OK);
    source.work.node().recover();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    auto &model = source.work.kernel()->work;
    auto record = source.saved(goal.result.object.id);
    const auto stamp = model.transitions.next();
    record.stamp = stamp;
    record.requirements[0].dependency = {grant.result.object.id, 1};
    ASSERT_TRUE(valid_goal_authority_record(record));
    const auto index = source.goal_index(goal.result.object.id);
    model.goals.authority_changed(index, stamp);
    std::vector<uint8_t> state(goal_state_record_size);
    write_goal_state_record(model.goals.at(index), state.data());
    std::vector<uint8_t> bytes(goal_authority_record_size(record));
    write_goal_authority_record(record, bytes.data());
    const BatchIdentity identity{source.work.kernel()->log.writer_epoch(), stamp.commit_seq,
                                 stamp.transition_id};
    WorkReplay replay(model);
    const RecordHeader state_header{goal_state_record_type,
                                    goal_state_with_authority_record_version, record_required,
                                    static_cast<uint32_t>(state.size())};
    ASSERT_EQ(WorkReplay::on_record(&replay, identity, state_header, state.data()),
              RecordUse::applied);
    const RecordHeader authority_header{goal_authority_record_type, goal_authority_record_version,
                                        record_required, static_cast<uint32_t>(bytes.size())};
    EXPECT_EQ(WorkReplay::on_record(&replay, identity, authority_header, bytes.data()),
              RecordUse::corrupt);
}

// Public-path setup shared by fault cases; only the later stored-byte edits
// are White-box. A new command always follows completed durable creation.
uint64_t approve_proposal(Fixture &source, uint64_t duration = 0) {
    const auto created =
        source.work.create(WorkNode::create_body(source.work.instance(agent_a), 0, "proposal"));
    EXPECT_EQ(created.status, AYNI_OK);
    if (created.status != AYNI_OK) {
        return 0;
    }
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), created.result.object.id);
    request.body.lifetime = duration ? AYNI_GRANT_CURRENT_BOOT : AYNI_GRANT_PERMANENT;
    request.body.duration_us = duration;
    auto intent =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 100;
    const auto result = source.work.submit(intent);
    EXPECT_EQ(result.status, AYNI_OK);
    source.work.node().recover();
    return result.status == AYNI_OK ? created.result.object.id : 0;
}

// C-STORE-019: the complete transition admits no unrelated records before,
// between, or after its own facts, even if the extra record is individually legal.
TEST(GoalApprovalPersistence, unrelated_work_before_during_or_after_approval_restricts_boot) {
    Fixture source;
    source.run();
    ASSERT_NE(approve_proposal(source), 0U);
    const auto image = source.work.node().sim().durable_storage();
    const auto original = decode(image.log.back());
    auto instance = *source.work.kernel()->work.agents.find_instance(source.work.instance(agent_b));
    instance.lifecycle = AYNI_AGENT_PAUSED;
    ++instance.revision;
    Record extra{{instance_record_type, instance_record_version, 0, instance_record_size},
                 std::vector<uint8_t>(instance_record_size)};
    write_instance_record(instance, extra.body.data());
    for (const size_t at : {size_t{0}, size_t{1}, original.records.size()}) {
        auto changed = original;
        changed.records.insert(changed.records.begin() + static_cast<std::ptrdiff_t>(at), extra);
        Fixture restored;
        restored.work.node().sim().durable_storage() = image;
        restored.work.node().sim().durable_storage().log.back() = encode(changed);
        restored.run();
        EXPECT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED) << at;
        EXPECT_EQ(restored.work.kernel()->work.grants.size(), 0U);
    }
}

// records.md type 9: a valid v4 body cannot be reinterpreted as any old version.
TEST(GoalApprovalPersistence, approval_kind_is_rejected_in_legacy_command_envelopes) {
    Fixture source;
    source.run();
    ASSERT_NE(approve_proposal(source), 0U);
    const auto batch = decode(source.work.node().sim().log().back());
    const auto &record = batch.records.back();
    for (uint16_t version = ayni::kernel::command_record_version;
         version < ayni::kernel::approval_command_record_version; ++version) {
        ayni::kernel::CommandMapping out{};
        uint64_t floor = 17;
        ayni_bytes_v1 canonical{};
        EXPECT_FALSE(ayni::kernel::read_command_record(
            version, record.body.data(), record.header.length, out, floor, canonical));
        EXPECT_EQ(floor, 17U);
    }
}

// C-STORE-019: a wider valid Grant can cover a need but cannot be the exact
// narrower permission management approved. Exercise journal and snapshot proof.
TEST(GoalApprovalPersistence, wider_same_length_bound_scope_is_not_the_approved_scope) {
    Fixture source;
    source.narrow_first = true;
    source.run();
    ASSERT_NE(approve_proposal(source), 0U);
    const auto storage = source.work.node().sim().durable_storage();
    auto batch = decode(storage.log.back());
    auto image = snapshot(source);
    auto &model = source.work.kernel()->work;
    ayni_authority_value_set_v1 values{};
    values.struct_size = sizeof(values);
    values.kind = AYNI_AUTHORITY_I64;
    values.unit = text("count");
    values.values.integer = {0, 20};
    const ayni_scope_field_v1 field{sizeof(field), 1, values};
    const ayni_scope_v1 scope{sizeof(scope), 1, &field};
    std::vector<uint8_t> bytes(model.grants.scope(0).size);
    uint32_t size = 0;
    ASSERT_EQ(ayni::authority::normalize_scope(model.grants.source_domain(0), scope, bytes.data(),
                                               static_cast<uint32_t>(bytes.size()), size),
              AYNI_OK);
    const ayni_bytes_v1 changed{bytes.data(), size};
    ayni::authority::write_grant_record(model.grants.at(0), model.grants.source_domain(0), changed,
                                        batch.records.front().body.data());
    for (auto &record : image.records) {
        if (record.header.type == ayni::authority::grant_record_type) {
            ayni::authority::write_grant_snapshot_record(model.grants.at(0),
                                                         model.grants.source_domain(0), changed,
                                                         record.body.data(), record.header.version);
        }
    }
    Fixture restored;
    restored.work.node().sim().durable_storage() = storage;
    restored.work.node().sim().durable_storage().log.back() = encode(batch);
    restored.run();
    EXPECT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    Fixture compacted;
    compacted.run();
    EXPECT_EQ(import_snapshot(compacted, image), BatchCheck::corrupt);
}

// C-STORE-019: compacted Grant duration must equal the saved approval command,
// even when both durations independently satisfy the lifetime shape contract.
TEST(GoalApprovalPersistence, snapshot_bound_duration_must_match_original_approval) {
    Fixture source;
    source.run();
    ASSERT_NE(approve_proposal(source, 1000000), 0U);
    auto image = snapshot(source);
    auto grant = source.work.kernel()->work.grants.at(0);
    ++grant.duration_us;
    for (auto &record : image.records) {
        if (record.header.type == ayni::authority::grant_record_type) {
            ayni::authority::write_grant_snapshot_record(
                grant, source.work.kernel()->work.grants.source_domain(0),
                source.work.kernel()->work.grants.scope(0), record.body.data(),
                record.header.version);
        }
    }
    Fixture restored;
    restored.run();
    EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
}

// C-STORE-019: two equal missing needs still require two distinct issued facts.
// Deleting one and reusing the other cannot disguise an incomplete batch.
TEST(GoalApprovalPersistence, one_new_grant_cannot_fill_two_equal_original_missing_items) {
    Fixture source;
    source.proposal_count = 2;
    source.work.node().config().grant_capacity = 2;
    source.run();
    ASSERT_NE(approve_proposal(source), 0U);
    const auto image = source.work.node().sim().durable_storage();
    auto batch = decode(image.log.back());
    auto &record = authority_record(batch.records);
    GoalAuthorityRecord metadata{};
    ASSERT_TRUE(read_goal_authority_record(record.body.data(), record.header.length, metadata));
    metadata.requirements[1].dependency = metadata.requirements[0].dependency;
    write_goal_authority_record(metadata, record.body.data());
    batch.records.erase(batch.records.begin() + 1);
    Fixture restored;
    restored.work.node().config().grant_capacity = 2;
    restored.work.node().sim().durable_storage() = image;
    restored.work.node().sim().durable_storage().log.back() = encode(batch);
    restored.run();
    EXPECT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
}

// C-COMMIT-003/005: actual maximum-count approval and its large domain/scope
// copies fit the reserved buffer. Verify the real writer, not a copied formula.
TEST(GoalApprovalPersistence, maximum_missing_count_with_large_domains_fits_batch_reservation) {
    Fixture source;
    source.proposal_count = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    source.work.node().config().grant_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    source.work.node().config().goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    source.work.node().config().authority_scope_max_bytes = 2560;
    source.work.node().config().storage_buffer_bytes = 0;
    source.run_with_named_targets(AYNI_AUTHORITY_FIELD_COUNT_MAX);
    ASSERT_NE(approve_proposal(source), 0U);
    EXPECT_EQ(source.work.kernel()->work.grants.size(), AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);
    const auto batch = decode(source.work.node().sim().log().back());
    EXPECT_EQ(batch.records.size(), AYNI_GOAL_REQUIREMENT_CAPACITY_MAX + 3U);
}

// C-AUTH-032/C-STORE-019: approval may not replace an original reference with a
// different older Grant, even when both equally cover the original requirement.
TEST(GoalApprovalPersistence, approval_metadata_cannot_replace_an_original_selected_grant) {
    Fixture source;
    source.narrow_first = true;
    source.proposal_count = 2;
    source.work.node().config().grant_capacity = 3;
    source.run();
    ayni_authority_value_set_v1 values{};
    values.struct_size = sizeof(values);
    values.kind = AYNI_AUTHORITY_I64;
    values.unit = text("count");
    values.values.integer = {0, 20};
    const ayni_scope_field_v1 field{sizeof(field), 1, values};
    const ayni_scope_v1 scope{sizeof(scope), 1, &field};
    const auto first = source.create_grant_for(source.work.instance(agent_a), scope);
    ASSERT_EQ(first.status, AYNI_OK);
    source.work.node().recover();
    const auto alternative = source.create_grant_for(source.work.instance(agent_a), scope);
    ASSERT_EQ(alternative.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = approve_proposal(source);
    ASSERT_NE(goal, 0U);
    const auto image = source.work.node().sim().durable_storage();
    auto batch = decode(image.log.back());
    auto &record = authority_record(batch.records);
    GoalAuthorityRecord metadata{};
    ASSERT_TRUE(read_goal_authority_record(record.body.data(), record.header.length, metadata));
    const auto selected = metadata.requirements[0].dependency.grant_id;
    metadata.requirements[0].dependency.grant_id =
        selected == first.result.object.id ? alternative.result.object.id : first.result.object.id;
    write_goal_authority_record(metadata, record.body.data());
    Fixture restored;
    restored.work.node().config().grant_capacity = 3;
    restored.work.node().sim().durable_storage() = image;
    restored.work.node().sim().durable_storage().log.back() = encode(batch);
    restored.run();
    EXPECT_EQ(restored.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
}

// C-STORE-019: historical proof permits a dependency to end after approval,
// but refuses an individually valid terminal fact that predates approval.
// White-box faults use real writers and a genuine earlier spare transition.
TEST(GoalApprovalPersistence, old_dependency_must_have_been_active_at_approval) {
    Fixture source;
    source.narrow_first = true;
    source.proposal_count = 2;
    source.work.node().config().grant_capacity = 2;
    source.run();
    ayni_authority_value_set_v1 values{};
    values.struct_size = sizeof(values);
    values.kind = AYNI_AUTHORITY_I64;
    values.unit = text("count");
    values.values.integer = {0, 20};
    const ayni_scope_field_v1 field{sizeof(field), 1, values};
    const ayni_scope_v1 scope{sizeof(scope), 1, &field};
    const auto generic = source.create_grant_for(source.work.instance(agent_a), scope);
    ASSERT_EQ(generic.status, AYNI_OK);
    source.work.node().recover();
    const auto spare = source.goal();
    const auto spare_stamp = source.work.kernel()->work.transitions.latest();
    ASSERT_NE(spare, 0U);
    ASSERT_NE(approve_proposal(source), 0U);
    const auto original = source.work.node().sim().durable_storage();
    auto invalid_snapshot = snapshot(source);
    auto terminal = source.work.kernel()->work.grants.at(0);
    terminal.lifecycle = AYNI_GRANT_EXPIRED;
    terminal.revision = ayni::authority::grant_terminal_revision;
    terminal.stamp = spare_stamp;
    for (auto &record : invalid_snapshot.records) {
        if (record.header.type != ayni::authority::grant_record_type) {
            continue;
        }
        uint64_t id = 0;
        ASSERT_TRUE(
            ayni::authority::read_grant_record_id(record.body.data(), record.header.length, id));
        if (id == generic.result.object.id) {
            ayni::authority::write_grant_snapshot_record(
                terminal, source.work.kernel()->work.grants.source_domain(0),
                source.work.kernel()->work.grants.scope(0), record.body.data(),
                record.header.version);
        }
    }
    Fixture compacted;
    compacted.work.node().config().grant_capacity = 2;
    compacted.run();
    EXPECT_EQ(import_snapshot(compacted, invalid_snapshot), BatchCheck::corrupt);
    auto corrupted = original;
    auto earlier = std::find_if(corrupted.log.begin(), corrupted.log.end(), [&](const auto &item) {
        return item.commit_seq == spare_stamp.commit_seq;
    });
    ASSERT_NE(earlier, corrupted.log.end());
    auto changed = decode(*earlier);
    Record state{{ayni::authority::grant_state_record_type,
                  ayni::authority::grant_state_record_version, record_required,
                  ayni::authority::grant_state_record_size},
                 std::vector<uint8_t>(ayni::authority::grant_state_record_size)};
    ayni::authority::write_grant_state_record(terminal, state.body.data());
    changed.records = {state};
    *earlier = encode(changed);
    Fixture replayed;
    replayed.work.node().config().grant_capacity = 2;
    replayed.work.node().sim().durable_storage() = corrupted;
    replayed.run();
    EXPECT_EQ(replayed.work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    // A later real revocation is valid history; its authority remains unusable.
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, generic.result.object.id};
    auto intent = source.work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
    intent.command_id = 101;
    ASSERT_EQ(source.work.submit(intent).status, AYNI_OK);
    source.work.node().recover();
    Fixture valid_snapshot;
    valid_snapshot.work.node().config().grant_capacity = 2;
    valid_snapshot.run();
    EXPECT_EQ(import_snapshot(valid_snapshot, snapshot(source)), BatchCheck::ok);
    Fixture valid_journal;
    valid_journal.work.node().config().grant_capacity = 2;
    valid_journal.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    valid_journal.run();
    EXPECT_EQ(valid_journal.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// ADR-0038 decision 12: a newly established binding cannot claim a Grant
// whose chain had already ended. White-box corruption keeps valid framing.
TEST(GoalAuthorityPersistence, initial_binding_rejects_an_already_expired_selection) {
    Fixture source;
    source.run();
    const auto grant = source.create_grant();
    ASSERT_EQ(grant.status, AYNI_OK);
    source.work.node().recover();
    source.expire();
    source.attach(source.goal(), grant.result.object.id);
    Fixture compacted;
    compacted.run();
    EXPECT_EQ(import_snapshot(compacted, snapshot(source)), BatchCheck::corrupt);
    Fixture replayed;
    replayed.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    replayed.run();
    EXPECT_EQ(replayed.work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
}

// ADR-0038 decision 12: first admission establishes the batch's own sequence,
// not another earlier, individually well-formed binding generation.
TEST(GoalAuthorityPersistence, fresh_creation_rejects_a_forged_earlier_binding) {
    Fixture source;
    source.run();
    source.goal();
    auto storage = source.work.node().sim().durable_storage();
    auto batch = decode(storage.log.back());
    auto &row = authority_record(batch.records);
    GoalAuthorityRecord record{};
    ASSERT_TRUE(read_goal_authority_record(row.body.data(), row.header.length, record));
    ASSERT_GT(record.binding.established_seq, 1U);
    record.binding.established_seq = 1;
    ASSERT_TRUE(valid_goal_authority_record(record));
    write_goal_authority_record(record, row.body.data());
    storage.log.back() = encode(batch);
    Fixture replayed;
    replayed.work.node().sim().durable_storage() = storage;
    replayed.run();
    EXPECT_EQ(replayed.work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
}

// ADR-0038 decision 12: F2 has no reauthorization transaction. Even a zero-needs
// Goal cannot acquire a forged input resume boundary in a static snapshot.
TEST(GoalAuthorityPersistence, snapshot_rejects_resume_boundaries_before_the_resume_protocol) {
    Fixture source;
    source.run();
    source.goal();
    for (const uint64_t through : {0U, 1U}) {
        auto image = snapshot(source);
        auto &row = authority_record(image.records);
        GoalAuthorityRecord record{};
        ASSERT_TRUE(read_goal_authority_record(row.body.data(), row.header.length, record));
        record.binding.input = {image.metadata.boot_epoch, through};
        ASSERT_TRUE(valid_goal_authority_record(record));
        write_goal_authority_record(record, row.body.data());
        Fixture restored;
        restored.run();
        EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
    }
}

// C-STORE-020: TASK_AUTHORITY cannot close a snapshot Task before its STATE.
// Individually valid admission bytes cannot substitute the table's default state.
TEST(TaskAuthorityPersistence, snapshot_cannot_skip_state_before_its_admission) {
    Fixture source;
    source.run();
    const auto goal = source.work.create(WorkNode::create_body(
        source.work.instance(agent_a), 0, "reminder", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(goal.status, AYNI_OK);
    source.work.node().step(1);
    source.work.node().recover();
    ASSERT_NE(source.work.goal(goal.result.object.id).last_task_id, 0U);
    auto image = snapshot(source);
    const auto state =
        std::find_if(image.records.begin(), image.records.end(),
                     [](const auto &row) { return row.header.type == task_state_record_type; });
    ASSERT_NE(state, image.records.end());
    --image.metadata.record_count;
    image.metadata.payload_length -= record_header_size + state->header.length;
    image.records.erase(state);
    Fixture restored;
    restored.run();
    EXPECT_EQ(import_snapshot(restored, image), BatchCheck::corrupt);
}

// ADR-0038: terminal progress does not waive its inherited admission proof.
// Each fault retains valid record shape/framing and targets cross-table evidence.
TEST(TaskAuthorityPersistence, terminal_snapshot_rejects_missing_or_substituted_admission) {
    Fixture source;
    source.work.node().config().grant_capacity = 4;
    source.run();
    const auto selected = source.create_grant();
    ASSERT_EQ(selected.status, AYNI_OK);
    source.work.node().recover();
    const auto alternative = source.create_grant();
    ASSERT_EQ(alternative.status, AYNI_OK);
    source.work.node().recover();
    const auto goal = source.work.create(WorkNode::create_body(
        source.work.instance(agent_a), 0, "proposal", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(goal.status, AYNI_OK);
    source.work.node().step();
    source.work.node().recover();
    const auto task = source.work.goal(goal.result.object.id).last_task_id;
    ASSERT_NE(task, 0U);
    source.work.node().step();
    source.work.node().recover();
    ASSERT_EQ(source.work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    const auto original = snapshot(source);
    for (unsigned fault = 0; fault < 3; ++fault) {
        SCOPED_TRACE(fault);
        auto changed = original;
        const auto row =
            std::find_if(changed.records.begin(), changed.records.end(), [](const auto &record) {
                return record.header.type == task_authority_record_type;
            });
        ASSERT_NE(row, changed.records.end());
        TaskAuthorityRecord record{};
        ASSERT_TRUE(read_task_authority_record(row->body.data(), row->header.length, record));
        ASSERT_EQ(record.count, 1U);
        ASSERT_EQ(record.references[0].grant_id, selected.result.object.id);
        if (fault == 0U) {
            --changed.metadata.record_count;
            changed.metadata.payload_length -= record_header_size + row->header.length;
            changed.records.erase(row);
        } else {
            if (fault == 1U) {
                record.references[0].grant_id = alternative.result.object.id;
            } else {
                --record.ceiling.commit_seq;
            }
            ASSERT_TRUE(valid_task_authority_record(record));
            write_task_authority_record(record, row->body.data());
        }
        Fixture restored;
        restored.work.node().config().grant_capacity = 4;
        restored.run();
        EXPECT_EQ(import_snapshot(restored, changed), BatchCheck::corrupt);
    }
}

// ADR-0038: CRC-valid metadata must prove the actual admission generation,
// exact parent generation and empty initial input frontier, including compaction.
TEST(GoalApprovalPersistence, forged_binding_history_is_rejected_in_journal_and_snapshot) {
    Fixture source;
    source.work.node().config().grant_capacity = 4;
    source.run();
    const auto parent = approve_proposal(source);
    ASSERT_NE(parent, 0U);
    const auto created = source.work.create(
        WorkNode::create_body(source.work.instance(agent_a), parent, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    const auto child = created.result.object.id;
    ASSERT_EQ(source.work.goal(child).lifecycle, AYNI_GOAL_PROPOSED);
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), child);
    auto intent =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    intent.command_id = 101;
    ASSERT_EQ(source.work.submit(intent).status, AYNI_OK);
    source.work.node().recover();
    const auto journal = source.work.node().sim().durable_storage();
    const auto compacted = snapshot(source);
    for (unsigned fault = 0; fault < 3; ++fault) {
        SCOPED_TRACE(fault);
        auto forge_binding = [fault, child](std::vector<Record> &records) {
            for (auto &row : records) {
                if (row.header.type != goal_authority_record_type) {
                    continue;
                }
                GoalAuthorityRecord record{};
                if (!read_goal_authority_record(row.body.data(), row.header.length, record)) {
                    ADD_FAILURE() << "source authority must decode";
                    return false;
                }
                if (record.goal_id != child) {
                    continue;
                }
                if (fault == 0U) {
                    --record.binding.established_seq;
                } else if (fault == 1U) {
                    --record.binding.parent.commit_seq;
                } else {
                    record.binding.input.incarnation = 1;
                }
                EXPECT_TRUE(valid_goal_authority_record(record));
                std::vector<uint8_t> bytes(row.body.size());
                write_goal_authority_record(record, bytes.data());
                row.body = std::move(bytes);
                return true;
            }
            return false;
        };
        auto changed_journal = journal;
        auto batch = decode(changed_journal.log.back());
        ASSERT_TRUE(forge_binding(batch.records));
        changed_journal.log.back() = encode(batch);
        Fixture replayed;
        replayed.work.node().config().grant_capacity = 4;
        replayed.work.node().sim().durable_storage() = changed_journal;
        replayed.run();
        EXPECT_EQ(replayed.work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);

        auto changed_snapshot = compacted;
        ASSERT_TRUE(forge_binding(changed_snapshot.records));
        Fixture restored;
        restored.work.node().config().grant_capacity = 4;
        restored.run();
        EXPECT_EQ(import_snapshot(restored, changed_snapshot), BatchCheck::corrupt);
    }
}

// C-STORE-018 / ADR-0038: historical parent admission survives legitimate
// terminal-reference release and Grant-slot reuse after the child was approved.
TEST(GoalApprovalPersistence, terminal_parent_history_survives_reclaimed_ancestor_grant) {
    Fixture source;
    source.work.node().config().grant_capacity = 3;
    source.work.node().config().command_capacity = 8;
    source.proposal_count = 2;
    source.narrow_first = true;
    source.run();
    ayni_authority_value_set_v1 range{};
    range.struct_size = sizeof(range);
    range.kind = AYNI_AUTHORITY_I64;
    range.unit = text("count");
    range.values.integer = {0, 10};
    const ayni_scope_field_v1 field{sizeof(field), 1, range};
    const ayni_scope_v1 narrow{sizeof(narrow), 1, &field};
    const auto generic = source.create_grant_for(source.work.instance(agent_a), narrow);
    ASSERT_EQ(generic.status, AYNI_OK);
    source.work.node().recover();
    const auto parent = approve_proposal(source);
    ASSERT_NE(parent, 0U);
    ASSERT_EQ(source.saved(parent).count, 2U);
    source.proposal_count = 1;
    source.narrow_first = false;
    const auto created = source.work.create(
        WorkNode::create_body(source.work.instance(agent_a), parent, "proposal"));
    ASSERT_EQ(created.status, AYNI_OK);
    const auto child = created.result.object.id;
    ASSERT_EQ(source.work.goal(child).lifecycle, AYNI_GOAL_PROPOSED);
    source.work.node().recover();
    ApprovalRequest request(source.work.kernel(), child);
    auto approval =
        source.work.intent(AYNI_INTENT_GOAL_APPROVE, &request.body, request.info.revision);
    approval.command_id = 101;
    ASSERT_EQ(source.work.submit(approval).status, AYNI_OK);
    source.work.node().recover();
    const auto binding = source.saved(child).binding;
    ASSERT_EQ(source.work.change(AYNI_INTENT_GOAL_CANCEL, parent).status, AYNI_OK);
    source.work.node().recover();
    ASSERT_EQ(source.work.goal(parent).lifecycle, AYNI_GOAL_CANCELLED);
    ASSERT_EQ(source.work.goal(child).lifecycle, AYNI_GOAL_CANCELLED);
    source.expire();

    const ayni_scope_v1 full{sizeof(full), 0, nullptr};
    const ayni_grant_create_v1 body{sizeof(body),
                                    1,
                                    text("device.move"),
                                    source.work.instance(agent_a),
                                    0,
                                    AYNI_GRANT_CURRENT_BOOT,
                                    0,
                                    Fixture::duration_us,
                                    full};
    auto grant = source.work.intent(AYNI_INTENT_GRANT_CREATE, &body);
    grant.command_id = 102;
    ASSERT_EQ(source.work.submit(grant).status, AYNI_OK);
    source.work.node().recover();
    uint32_t index = 0;
    ASSERT_FALSE(source.work.kernel()->work.grants.find(generic.result.object.id, index));
    const auto compacted = snapshot(source);
    Fixture restored;
    restored.work.node().config().grant_capacity = 3;
    restored.work.node().config().command_capacity = 8;
    restored.run();
    ASSERT_EQ(import_snapshot(restored, compacted), BatchCheck::ok);
    EXPECT_EQ(restored.saved(child).binding.parent.commit_seq, binding.parent.commit_seq);

    Fixture rebooted;
    rebooted.work.node().config().grant_capacity = 3;
    rebooted.work.node().config().command_capacity = 8;
    rebooted.work.node().sim().durable_storage() = source.work.node().sim().durable_storage();
    rebooted.run();
    ASSERT_EQ(rebooted.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(rebooted.saved(child).binding.parent.commit_seq, binding.parent.commit_seq);
}

} // namespace
