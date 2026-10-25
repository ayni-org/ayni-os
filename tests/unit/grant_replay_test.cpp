// Faults below are complete, CRC-valid batches or framed snapshot records.
// They exercise semantic recovery checks rather than checksum rejection.
#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "durability/batch/batch_codec.h"
#include "foundation/little_endian.h"
#include "kernel/boot/boot_sequence.h"
#include "kernel/command/command_records.h"
#include "kernel/persistence/work_snapshot.h"
#include "work_node.h"

#include <ayni/authority.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using ayni::kernel::KernelTestProbe;

namespace {
using namespace ayni::test;
using namespace ayni::durability;
using ayni::authority::GrantRecord;
using ayni::hosts::sim::StoredBatch;
using ayni::kernel::CommandMapping;

struct Record {
    RecordHeader header;
    std::vector<uint8_t> body;
};

struct Batch {
    BatchIdentity identity;
    uint16_t schema;
    std::vector<Record> records;
};

Batch decode(const StoredBatch &stored) {
    BatchView view{};
    EXPECT_FALSE(stored.torn);
    EXPECT_EQ(read_batch(stored.bytes.data(), stored.bytes.size(), view), BatchCheck::ok);
    Batch result{view.identity, view.schema, {}};
    RecordReader reader(view);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    for (;;) {
        const auto next = reader.next(header, body);
        if (next == RecordReader::Next::end) {
            break;
        }
        EXPECT_EQ(next, RecordReader::Next::record);
        if (next != RecordReader::Next::record) {
            break;
        }
        result.records.push_back({header, {body, body + header.length}});
    }
    return result;
}

StoredBatch encode(const Batch &batch) {
    std::size_t body_size = 0;
    for (const auto &record : batch.records) {
        body_size += record.body.size();
    }
    StoredBatch stored{};
    stored.commit_seq = batch.identity.commit_seq;
    stored.bytes.resize(batch_size(body_size, batch.records.size()));
    BatchWriter writer(stored.bytes.data(), stored.bytes.size(), batch.identity, batch.schema);
    for (const auto &record : batch.records) {
        EXPECT_EQ(record.header.length, record.body.size());
        EXPECT_TRUE(writer.add_record(record.header, record.body.data()));
    }
    EXPECT_EQ(writer.finish(), stored.bytes.size());
    BatchView checked{};
    EXPECT_EQ(read_batch(stored.bytes.data(), stored.bytes.size(), checked), BatchCheck::ok);
    return stored;
}

class ReplayNode {
  public:
    ReplayNode() {
        node.config().grant_capacity = 2;
        node.config().command_capacity = 8;
        node.on_create([](ayni_kernel_t *kernel) {
            const auto owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
            ayni_authority_value_set_v1 values{};
            values.struct_size = sizeof(values);
            values.kind = AYNI_AUTHORITY_I64;
            values.unit = text("count");
            values.values.integer = {0, 100};
            const ayni_authority_field_desc_v1 field{sizeof(field), 1, AYNI_AUTHORITY_PARAMETER, 0,
                                                     values};
            const ayni_capability_desc_v1 capability{
                sizeof(capability), 1, text("device.move"), 1, 0, &field};
            ASSERT_EQ(ayni_register_capability(kernel, &capability), AYNI_OK);
        });
    }
    void run() {
        node.run();
        const auto descriptor = session_desc();
        ASSERT_EQ(ayni_open_session(node.kernel(), &descriptor, &session_), AYNI_OK);
    }
    Submitted create(uint64_t command, int64_t minimum = 10, int64_t maximum = 20,
                     uint64_t parent = 0, uint64_t duration = 0, uint32_t flags = 0) {
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        EXPECT_EQ(ayni_get_agent(node.kernel(), agent_a, &agent), AYNI_OK);
        ayni_authority_value_set_v1 values{};
        values.struct_size = sizeof(values);
        values.kind = AYNI_AUTHORITY_I64;
        values.unit = text("count");
        values.values.integer = {minimum, maximum};
        const ayni_scope_field_v1 field{sizeof(field), 1, values};
        const ayni_grant_create_v1 body{sizeof(body),
                                        1,
                                        text("device.move"),
                                        agent.instance_id,
                                        parent,
                                        duration == 0U ? AYNI_GRANT_PERMANENT
                                                       : AYNI_GRANT_CURRENT_BOOT,
                                        flags,
                                        duration,
                                        {sizeof(ayni_scope_v1), 1, &field}};
        const ayni_intent_v1 intent{sizeof(intent), AYNI_INTENT_GRANT_CREATE, command,
                                    session_,       parent == 0U ? 0U : 1U,   0,
                                    &body};
        Submitted result{};
        result.result.struct_size = sizeof(result.result);
        result.error.struct_size = sizeof(result.error);
        result.status = ayni_submit_intent(node.kernel(), &intent, &result.result, &result.error);
        return result;
    }
    Submitted revoke(uint64_t command, uint64_t target, uint32_t revision = 1) {
        const ayni_grant_revoke_v1 body{sizeof(body), 0, target};
        const ayni_intent_v1 intent{
            sizeof(intent), AYNI_INTENT_GRANT_REVOKE, command, session_, revision, 0, &body};
        Submitted result{};
        result.result.struct_size = sizeof(result.result);
        result.error.struct_size = sizeof(result.error);
        result.status = ayni_submit_intent(node.kernel(), &intent, &result.result, &result.error);
        return result;
    }
    Submitted change_instance(ayni_intent_kind_t kind, uint64_t command, uint32_t revision) {
        ayni_agent_info_v1 agent{};
        agent.struct_size = sizeof(agent);
        EXPECT_EQ(ayni_get_agent(node.kernel(), agent_a, &agent), AYNI_OK);
        const ayni_instance_target_v1 target{sizeof(target), 0, agent.instance_id};
        const ayni_intent_v1 intent{sizeof(intent), kind, command, session_, revision, 0, &target};
        Submitted result{};
        result.result.struct_size = sizeof(result.result);
        result.error.struct_size = sizeof(result.error);
        result.status = ayni_submit_intent(node.kernel(), &intent, &result.result, &result.error);
        return result;
    }
    void store(uint64_t sequence) {
        for (unsigned round = 0; round < 1000 && node.info().durable_seq < sequence; ++round) {
            const auto report = node.step(1);
            const auto completed = node.sim().complete_storage();
            if (node.info().durable_seq >= sequence) {
                break;
            }
            if (completed != 0U) {
                continue; // apply the posted completion before advancing time
            }
            if (report.more_ready == 0U && node.sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > node.sim().now_us()) {
                node.sim().advance_us(report.next_deadline_us - node.sim().now_us());
            }
        }
        ASSERT_GE(node.info().durable_seq, sequence);
    }
    ayni_grant_info_v1 grant(uint64_t id) {
        ayni_grant_info_v1 result{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(ayni_get_grant(node.kernel(), id, &result), AYNI_OK);
        return result;
    }
    void expire(uint64_t id) {
        const uint64_t deadline = grant(id).deadline_us;
        ASSERT_GE(deadline, node.sim().now_us());
        node.sim().advance_us(deadline - node.sim().now_us());
        for (unsigned round = 0; round < 1000 && grant(id).lifecycle == AYNI_GRANT_ACTIVE;
             ++round) {
            node.step(1);
        }
        ASSERT_EQ(grant(id).lifecycle, AYNI_GRANT_EXPIRED);
        store(grant(id).commit_seq);
    }
    Node node;

  private:
    uint64_t session_ = 0;
};

std::vector<StoredBatch> complete_log(bool with_child = false, bool terminal = false) {
    ReplayNode source;
    source.run();
    const auto root =
        source.create(1, 10, 20, 0, terminal ? 10000000U : 0U, AYNI_GRANT_ALLOW_DELEGATION);
    EXPECT_EQ(root.status, AYNI_OK);
    source.store(root.result.commit_seq);
    if (with_child) {
        const auto child = source.create(2, 11, 19, root.result.object.id);
        EXPECT_EQ(child.status, AYNI_OK);
        source.store(child.result.commit_seq);
    }
    if (terminal) {
        source.expire(root.result.object.id);
    }
    return source.node.sim().log();
}

template <typename Edit>
std::vector<StoredBatch> edited_log(const std::vector<StoredBatch> &original, Edit edit) {
    std::vector<StoredBatch> result;
    for (const auto &stored : original) {
        auto batch = decode(stored);
        edit(batch);
        result.push_back(encode(batch));
    }
    return result;
}

void expect_restricted(const std::vector<StoredBatch> &log, ayni_restriction_t reason) {
    ReplayNode restored;
    restored.node.sim().log() = log;
    restored.node.create_and_start();
    restored.node.recover();
    EXPECT_EQ(restored.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(restored.node.info().restriction, reason);
    EXPECT_EQ(KernelTestProbe::work(restored.node.kernel()).grants.size(), 0U);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, a_valid_but_expanded_create_scope_cannot_disagree_with_its_exact_command) {
    const auto original = complete_log();
    bool changed = false;
    const auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type != ayni::authority::grant_record_type) {
                continue;
            }
            GrantRecord decoded{};
            ASSERT_TRUE(ayni::authority::read_grant_record(record.body.data(), record.header.length,
                                                           decoded));
            const auto scope_at = static_cast<std::size_t>(decoded.scope.data - record.body.data());
            // Independent B format-1 offsets: 4-byte domain header,
            // 8-byte field header, five-byte "count", then two i64 values.
            ayni::foundation::store_le64(record.body.data() + scope_at + 17U, 0);
            ayni::foundation::store_le64(record.body.data() + scope_at + 25U, 100);
            ASSERT_TRUE(ayni::authority::read_grant_record(record.body.data(), record.header.length,
                                                           decoded));
            changed = true;
        }
    });
    ASSERT_TRUE(changed);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, child_parent_revision_must_match_the_persisted_exact_request) {
    const auto original = complete_log(true);
    bool changed = false;
    const auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type != ayni::kernel::command_record_type) {
                continue;
            }
            CommandMapping mapping{};
            uint64_t floor = 0;
            ayni_bytes_v1 canonical{};
            ASSERT_TRUE(ayni::kernel::read_command_record(record.header.version, record.body.data(),
                                                          record.header.length, mapping, floor,
                                                          canonical));
            ayni::kernel::AuthorityCommandRecord request{};
            ASSERT_TRUE(ayni::kernel::read_authority_command(canonical, request));
            if (request.parent_grant_id == 0U) {
                continue;
            }
            std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
            ayni::foundation::store_le32(bytes.data() + 4U, 2); // wrong expected parent revision
            const ayni_bytes_v1 changed_bytes{bytes.data(), static_cast<uint32_t>(bytes.size())};
            mapping.digest = ayni::kernel::authority_command_digest(changed_bytes);
            ASSERT_TRUE(ayni::kernel::write_command_record(
                mapping, floor, changed_bytes, record.body.data(), record.header.length));
            changed = true;
        }
    });
    ASSERT_TRUE(changed);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, neither_a_create_nor_an_exact_mapping_may_replay_without_its_partner) {
    const auto original = complete_log();
    for (const uint16_t removed :
         {ayni::authority::grant_record_type, ayni::kernel::command_record_type}) {
        const auto log = edited_log(original, [&](Batch &batch) {
            batch.records.erase(std::remove_if(batch.records.begin(), batch.records.end(),
                                               [removed](const Record &record) {
                                                   return record.header.type == removed;
                                               }),
                                batch.records.end());
        });
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, pairing_cannot_cross_a_committed_batch_boundary) {
    auto log = complete_log();
    auto source = decode(log.back());
    Batch later{{source.identity.writer_epoch, source.identity.commit_seq + 1U,
                 source.identity.transition_id + 1U},
                source.schema,
                {}};
    for (const auto &record : source.records) {
        if (record.header.type == ayni::kernel::command_record_type) {
            later.records.push_back(record);
        }
    }
    ASSERT_EQ(later.records.size(), 1U);
    source.records.erase(std::remove_if(source.records.begin(), source.records.end(),
                                        [](const Record &record) {
                                            return record.header.type ==
                                                   ayni::kernel::command_record_type;
                                        }),
                         source.records.end());
    log.back() = encode(source);
    log.push_back(encode(later));
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, one_committed_batch_cannot_contain_two_complete_root_create_mapping_pairs) {
    ReplayNode source;
    source.run();
    const auto first = source.create(1);
    ASSERT_EQ(first.status, AYNI_OK);
    source.store(first.result.commit_seq);
    const auto second = source.create(2, 30, 40);
    ASSERT_EQ(second.status, AYNI_OK);
    source.store(second.result.commit_seq);
    auto log = source.node.sim().log();
    ASSERT_GE(log.size(), 2U);
    auto combined = decode(log[log.size() - 2U]);
    const auto later = decode(log.back());
    ASSERT_EQ(combined.records.size(), 2U);
    ASSERT_EQ(later.records.size(), 2U);
    // Both roots are independently legal and have no parent-stamp ordering
    // to reject this fault; only transition/command cardinality distinguishes it.
    combined.records.insert(combined.records.end(), later.records.begin(), later.records.end());
    log[log.size() - 2U] = encode(combined);
    log.pop_back();
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, an_ordinary_mapping_before_an_auth_pair_cannot_share_its_transition) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 2, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    auto log = source.node.sim().log();
    ASSERT_GE(log.size(), 2U);
    const auto authority = decode(log[log.size() - 2U]);
    auto combined = decode(log.back());
    combined.identity = authority.identity;
    // A PAUSED subject can receive a Grant. Its lifecycle therefore does not
    // hide the illegal second command in this otherwise valid same-stamp batch.
    combined.records.insert(combined.records.end(), authority.records.begin(),
                            authority.records.end());
    log[log.size() - 2U] = encode(combined);
    log.pop_back();
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, two_ordinary_mappings_cannot_claim_one_transition_either) {
    ReplayNode source;
    source.run();
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 1, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    auto log = source.node.sim().log();
    auto combined = decode(log.back());
    const auto found =
        std::find_if(combined.records.begin(), combined.records.end(), [](const Record &record) {
            return record.header.type == ayni::kernel::command_record_type;
        });
    ASSERT_NE(found, combined.records.end());
    auto duplicate = *found;
    CommandMapping mapping{};
    uint64_t floor = 0;
    ASSERT_TRUE(ayni::kernel::read_command_record(duplicate.body.data(), duplicate.header.length,
                                                  mapping, floor));
    mapping.command_id = 2;
    ayni::kernel::write_command_record(mapping, floor, duplicate.body.data());
    combined.records.push_back(duplicate);
    log.back() = encode(combined);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, new_authority_records_require_the_flag_even_for_unknown_future_versions) {
    const auto original = complete_log(false, true);
    for (const uint16_t type :
         {ayni::authority::grant_record_type, ayni::authority::grant_state_record_type,
          ayni::kernel::command_record_type}) {
        for (const bool future : {false, true}) {
            for (const bool required : {false, true}) {
                bool changed = false;
                const auto log = edited_log(original, [&](Batch &batch) {
                    for (auto &record : batch.records) {
                        if (record.header.type == type) {
                            record.header.flags = required ? record_required : 0;
                            record.header.version = future ? uint16_t{99} : record.header.version;
                            changed = true;
                        }
                    }
                });
                ASSERT_TRUE(changed);
                if (!required || future) {
                    expect_restricted(log, required ? AYNI_RESTRICTION_LOG_UNSUPPORTED
                                                    : AYNI_RESTRICTION_LOG_CORRUPT);
                }
                const RecordHeader header{
                    type,
                    static_cast<uint16_t>(
                        future ? 99U : (type == ayni::kernel::command_record_type ? 2U : 1U)),
                    required ? record_required : 0U, 0};
                EXPECT_EQ(ayni::kernel::WorkReplay::classify_header(header),
                          !required ? RecordUse::corrupt
                          : future  ? RecordUse::unknown
                                    : RecordUse::applied);
            }
        }
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, a_future_boot_grant_id_is_corrupt_even_when_mapping_and_create_agree) {
    const auto original = complete_log();
    const auto log = edited_log(original, [](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type == ayni::authority::grant_record_type) {
                const uint64_t id = ayni::foundation::load_le64(record.body.data());
                ayni::foundation::store_le64(record.body.data(), id + (UINT64_C(1) << 32U));
            } else if (record.header.type == ayni::kernel::command_record_type) {
                CommandMapping mapping{};
                uint64_t floor = 0;
                ayni_bytes_v1 canonical{};
                ASSERT_TRUE(ayni::kernel::read_command_record(
                    record.header.version, record.body.data(), record.header.length, mapping, floor,
                    canonical));
                mapping.object.id += UINT64_C(1) << 32U;
                std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
                ASSERT_TRUE(ayni::kernel::write_command_record(
                    mapping, floor, {bytes.data(), static_cast<uint32_t>(bytes.size())},
                    record.body.data(), record.header.length));
            }
        }
    });
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-ID-010 / records.md type 13/v1: a create uses the immediately durable
// boot, not an older ID that happens to remain below the final boot bound.
// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, an_earlier_boot_create_is_corrupt_even_when_mapping_and_create_agree) {
    ReplayNode first;
    first.run();
    ReplayNode second;
    second.node.sim().log() = first.node.sim().log();
    second.run();
    ASSERT_EQ(second.node.info().boot_epoch, 2U);
    const auto created = second.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    second.store(created.result.commit_seq);
    const auto original = second.node.sim().log();
    ReplayNode valid;
    valid.node.sim().log() = original;
    valid.run();
    ASSERT_EQ(valid.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    uint32_t edited_ids = 0;
    const auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type == ayni::authority::grant_record_type) {
                const auto id = ayni::foundation::load_le64(record.body.data());
                ayni::foundation::store_le64(record.body.data(), id - (UINT64_C(1) << 32U));
                ++edited_ids;
            } else if (record.header.type == ayni::kernel::command_record_type) {
                CommandMapping mapping{};
                uint64_t floor = 0;
                ayni_bytes_v1 canonical{};
                ASSERT_TRUE(ayni::kernel::read_command_record(
                    record.header.version, record.body.data(), record.header.length, mapping, floor,
                    canonical));
                mapping.object.id -= UINT64_C(1) << 32U;
                std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
                ASSERT_TRUE(ayni::kernel::write_command_record(
                    mapping, floor, {bytes.data(), static_cast<uint32_t>(bytes.size())},
                    record.body.data(), record.header.length));
                ++edited_ids;
            }
        }
    });
    ASSERT_EQ(edited_ids, 2U);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// ADR-0031 / records.md type 15/v1: revocation may target a permanent Grant
// from an earlier boot, and its original accepted result survives replay.
TEST(GrantRevokeReplay, revocation_of_a_prior_boot_grant_replays_with_its_original_result) {
    ReplayNode first;
    first.run();
    const auto created = first.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    first.store(created.result.commit_seq);
    ReplayNode second;
    second.node.sim().log() = first.node.sim().log();
    second.run();
    ASSERT_EQ(second.node.info().boot_epoch, 2U);
    const auto revoked = second.revoke(2, created.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    second.store(revoked.result.commit_seq);
    ReplayNode restored;
    restored.node.sim().log() = second.node.sim().log();
    restored.run();
    ASSERT_EQ(restored.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const auto grant = restored.grant(created.result.object.id);
    EXPECT_EQ(grant.lifecycle, AYNI_GRANT_REVOKED);
    EXPECT_EQ(grant.revision, 2U);
    const auto retried = restored.revoke(2, created.result.object.id);
    ASSERT_EQ(retried.status, AYNI_OK);
    EXPECT_EQ(retried.result.object.id, created.result.object.id);
    EXPECT_EQ(retried.result.object.revision, 2U);
}

struct Snapshot {
    SnapshotMetadata metadata;
    std::vector<Record> records;
};

Snapshot snapshot_of(ReplayNode &source) {
    // White-box: snapshot module export/import owns the durable model here;
    // no authorization or recovery gate is bypassed in public fixtures.
    auto &model = KernelTestProbe::work(source.node.kernel());
    const auto layout = ayni::kernel::measure_snapshot(model);
    Snapshot result{};
    result.metadata.generation = 1;
    result.metadata.covers_commit_seq = model.transitions.latest().commit_seq;
    result.metadata.max_transition_id = model.transitions.latest().transition_id;
    result.metadata.boot_epoch = source.node.info().boot_epoch;
    result.metadata.writer_epoch = 1;
    result.metadata.payload_length = layout.payload_length;
    result.metadata.record_count = layout.record_count;
    ayni::kernel::WorkSnapshotWriter writer(model);
    std::vector<uint8_t> page(65536);
    for (;;) {
        uint32_t size = 0;
        const auto next = writer.next(page.data(), static_cast<uint32_t>(page.size()), size);
        EXPECT_NE(next, ayni::kernel::WorkSnapshotWriter::Page::no_room);
        if (next != ayni::kernel::WorkSnapshotWriter::Page::page) {
            break;
        }
        uint32_t at = 0;
        while (at < size) {
            const RecordHeader header = read_record_header(page.data() + at);
            at += static_cast<uint32_t>(record_header_size);
            EXPECT_LE(header.length, size - at);
            result.records.push_back(
                {header, {page.data() + at, page.data() + at + header.length}});
            at += header.length;
        }
        EXPECT_EQ(at, size);
    }
    EXPECT_EQ(result.records.size(), layout.record_count);
    return result;
}

BatchCheck import_snapshot(const Snapshot &snapshot,
                           const std::function<void(ayni::kernel::WorkModel &)> &verify = {}) {
    ReplayNode restored;
    restored.node.create();
    auto metadata = snapshot.metadata;
    metadata.record_count = static_cast<uint32_t>(snapshot.records.size());
    ayni::kernel::WorkSnapshotReader reader(KernelTestProbe::work(restored.node.kernel()),
                                            metadata);
    for (const auto &record : snapshot.records) {
        const auto check = reader.apply(record.header, record.body.data());
        if (check != BatchCheck::ok) {
            return check;
        }
    }
    const auto check = reader.complete();
    if (check == BatchCheck::ok && verify) {
        verify(KernelTestProbe::work(restored.node.kernel()));
    }
    return check;
}

bool auth_mapping(const Record &record) {
    return record.header.type == ayni::kernel::command_record_type &&
           record.header.version == ayni::kernel::exact_command_record_version;
}

void rewrite_auth_request(Record &record, const std::function<void(std::vector<uint8_t> &)> &edit) {
    CommandMapping mapping{};
    uint64_t floor = 0;
    ayni_bytes_v1 canonical{};
    const uint32_t base = record.header.length - 8U;
    ASSERT_TRUE(ayni::kernel::read_command_record(record.header.version, record.body.data(), base,
                                                  mapping, floor, canonical));
    std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
    edit(bytes);
    const ayni_bytes_v1 view{bytes.data(), static_cast<uint32_t>(bytes.size())};
    mapping.digest = ayni::kernel::authority_command_digest(view);
    ASSERT_TRUE(ayni::kernel::write_command_record(mapping, floor, view, record.body.data(), base));
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_auth_mapping_requires_binding_identity_and_strictly_later_acceptance) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    auto original = snapshot_of(source);
    uint64_t binding_seq = 0;
    for (const auto &record : original.records) {
        if (record.header.type == ayni::authority::owner_binding_record_type) {
            binding_seq = ayni::foundation::load_le64(record.body.data() + 24U);
        }
    }
    ASSERT_GT(binding_seq, 1U);
    // Missing targets are allowed for retained command results. Removing the
    // target here isolates owner/sequence checks from retained-target matching.
    original.records.erase(std::remove_if(original.records.begin(), original.records.end(),
                                          [](const Record &record) {
                                              return record.header.type ==
                                                     ayni::authority::grant_record_type;
                                          }),
                           original.records.end());
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    for (const uint32_t fault : {0U, 1U, 2U}) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (!auth_mapping(record)) {
                continue;
            }
            if (fault == 0U) {
                ayni::foundation::store_le64(record.body.data(), owner_id + 1U);
            } else {
                ayni::foundation::store_le64(record.body.data() + record.body.size() - 8U,
                                             fault == 1U ? binding_seq : binding_seq - 1U);
            }
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << fault;
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_ordinary_v1_foreign_principal_remains_a_readable_retry_window) {
    ReplayNode source;
    source.run();
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 1, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    auto snapshot = snapshot_of(source);
    bool changed = false;
    for (auto &record : snapshot.records) {
        if (record.header.type == ayni::kernel::command_record_type) {
            ASSERT_EQ(record.header.version, ayni::kernel::command_record_version);
            ayni::foundation::store_le64(record.body.data(), owner_id + 1U);
            changed = true;
        }
    }
    ASSERT_TRUE(changed);
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::ok);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_auth_result_requires_nonzero_epoch_counter_and_no_future_boot) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    const auto original = snapshot_of(source);
    for (const uint64_t id : {UINT64_C(5), UINT64_C(1) << 32U, (UINT64_C(2) << 32U) | 5U}) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (auth_mapping(record)) {
                // v1/v2 share the fixed result reference at byte 40. The
                // complete canonical body and its digest stay valid.
                ayni::foundation::store_le64(record.body.data() + 40U, id);
            }
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << id;
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_retained_grant_must_match_immutable_request_and_created_sequence) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 2, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    auto original = snapshot_of(source);
    original.records.erase(std::remove_if(original.records.begin(), original.records.end(),
                                          [](const Record &record) {
                                              return record.header.type ==
                                                         ayni::kernel::command_record_type &&
                                                     !auth_mapping(record);
                                          }),
                           original.records.end());
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    for (uint32_t fault = 0; fault < 8; ++fault) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (!auth_mapping(record)) {
                continue;
            }
            if (fault == 7U) {
                ayni::foundation::store_le64(record.body.data() + record.body.size() - 8U,
                                             paused.result.commit_seq);
                continue;
            }
            rewrite_auth_request(record, [fault](std::vector<uint8_t> &bytes) {
                // Independent raw request format: 16-byte prefix, ID,
                // subject/parent, lifetime/flags, duration, length+scope.
                const uint32_t subject_at = 16U + ayni::foundation::load_le32(bytes.data() + 12U);
                switch (fault) {
                case 0:
                    bytes[16] = 'x';
                    break;
                case 1:
                    ayni::foundation::store_le32(bytes.data() + 8U, 2);
                    break;
                case 2:
                    ayni::foundation::store_le64(bytes.data() + subject_at, UINT64_C(0x100000099));
                    break;
                case 3:
                    ayni::foundation::store_le32(bytes.data() + 4U, 1);
                    ayni::foundation::store_le64(bytes.data() + subject_at + 8U,
                                                 UINT64_C(0x100000099));
                    break;
                case 4:
                    ayni::foundation::store_le32(bytes.data() + subject_at + 20U,
                                                 AYNI_GRANT_ALLOW_DELEGATION);
                    break;
                case 5:
                    ayni::foundation::store_le32(bytes.data() + subject_at + 16U,
                                                 AYNI_GRANT_CURRENT_BOOT);
                    ayni::foundation::store_le64(bytes.data() + subject_at + 24U, 10);
                    break;
                default:
                    // raw scope starts after subject..duration and its u32 length.
                    ayni::foundation::store_le64(bytes.data() + subject_at + 36U + 17U, 11);
                    break;
                }
            });
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << fault;
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_preserves_child_mapping_when_parent_later_expires_and_changes_revision) {
    ReplayNode source;
    source.run();
    const auto parent = source.create(1, 10, 20, 0, 10000000, AYNI_GRANT_ALLOW_DELEGATION);
    ASSERT_EQ(parent.status, AYNI_OK);
    source.store(parent.result.commit_seq);
    const auto child = source.create(2, 11, 19, parent.result.object.id, 1000000);
    ASSERT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    source.expire(parent.result.object.id);
    ASSERT_EQ(source.grant(parent.result.object.id).revision, 2U);
    EXPECT_EQ(import_snapshot(snapshot_of(source)), BatchCheck::ok);
    // A normal boot must replay already stored GRANT_STATE records too.
    source.node.reboot();
    source.node.recover();
    ASSERT_EQ(source.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(source.grant(parent.result.object.id).lifecycle, AYNI_GRANT_EXPIRED);
    EXPECT_EQ(source.grant(child.result.object.id).lifecycle, AYNI_GRANT_EXPIRED);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_reclaimed_result_restores_without_recreating_the_grant) {
    ReplayNode source;
    source.node.config().grant_capacity = 1;
    source.run();
    const auto created = source.create(1, 10, 20, 0, 10000000);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    const auto original = source.grant(created.result.object.id);
    source.expire(created.result.object.id);
    const auto replacement = source.create(2);
    ASSERT_EQ(replacement.status, AYNI_OK);
    source.store(replacement.result.commit_seq);
    ayni_authority_value_set_v1 values{};
    values.struct_size = sizeof(values);
    values.kind = AYNI_AUTHORITY_I64;
    values.unit = text("count");
    values.values.integer = {10, 20};
    const ayni_scope_field_v1 field{sizeof(field), 1, values};
    const ayni_grant_create_v1 request{sizeof(request),
                                       1,
                                       text("device.move"),
                                       original.subject_instance_id,
                                       0,
                                       AYNI_GRANT_CURRENT_BOOT,
                                       0,
                                       10000000,
                                       {sizeof(ayni_scope_v1), 1, &field}};
    EXPECT_EQ(import_snapshot(snapshot_of(source),
                              [&](ayni::kernel::WorkModel &model) {
                                  uint32_t index = 0;
                                  EXPECT_FALSE(model.grants.find(created.result.object.id, index));
                                  const auto retry =
                                      model.commands.find_exact(owner_id, 1, {0, &request});
                                  ASSERT_EQ(retry.status, AYNI_OK);
                                  ASSERT_EQ(retry.lookup,
                                            ayni::kernel::CommandTable::Lookup::retried);
                                  ASSERT_NE(retry.mapping, nullptr);
                                  EXPECT_EQ(retry.mapping->object.id, created.result.object.id);
                                  EXPECT_EQ(retry.mapping->commit_seq, created.result.commit_seq);
                              }),
              BatchCheck::ok);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_orders_a_reused_child_slot_after_its_parent_and_rejects_the_reverse) {
    ReplayNode source;
    source.run();
    const auto first = source.create(1, 10, 20, 0, 10000000);
    ASSERT_EQ(first.status, AYNI_OK);
    source.store(first.result.commit_seq);
    const auto parent = source.create(2, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
    ASSERT_EQ(parent.status, AYNI_OK);
    source.store(parent.result.commit_seq);
    source.expire(first.result.object.id);
    const auto child = source.create(3, 11, 19, parent.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    ASSERT_EQ(KernelTestProbe::work(source.node.kernel()).grants.at(0).grant_id,
              child.result.object.id);
    ASSERT_EQ(KernelTestProbe::work(source.node.kernel()).grants.at(1).grant_id,
              parent.result.object.id);
    auto snapshot = snapshot_of(source);
    std::vector<uint32_t> grant_indices;
    for (uint32_t index = 0; index < snapshot.records.size(); ++index) {
        if (snapshot.records[index].header.type == ayni::authority::grant_record_type) {
            grant_indices.push_back(index);
        }
    }
    ASSERT_EQ(grant_indices.size(), 2U);
    EXPECT_EQ(ayni::foundation::load_le64(snapshot.records[grant_indices[0]].body.data()),
              parent.result.object.id);
    EXPECT_EQ(ayni::foundation::load_le64(snapshot.records[grant_indices[1]].body.data()),
              child.result.object.id);
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::ok);
    std::swap(snapshot.records[grant_indices[0]], snapshot.records[grant_indices[1]]);
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt);
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, snapshot_requires_the_owner_and_bounds_both_stamps_and_grant_boot_epochs) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    const auto original = snapshot_of(source);
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    auto missing_owner = original;
    missing_owner.records.erase(
        std::remove_if(missing_owner.records.begin(), missing_owner.records.end(),
                       [](const Record &record) {
                           return record.header.type == ayni::authority::owner_binding_record_type;
                       }),
        missing_owner.records.end());
    EXPECT_EQ(import_snapshot(missing_owner), BatchCheck::corrupt);
    for (const uint32_t fault : {0U, 1U, 2U, 3U}) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::authority::grant_record_type) {
                continue;
            }
            const auto tail = record.body.size() - (record.header.version == 1U ? 40U : 80U);
            if (fault == 0U) {
                const uint64_t id = ayni::foundation::load_le64(record.body.data());
                ayni::foundation::store_le64(record.body.data(), id + (UINT64_C(1) << 32U));
            } else if (fault == 1U) {
                ayni::foundation::store_le64(record.body.data() + tail,
                                             snapshot.metadata.max_transition_id + 1U);
            } else if (fault == 2U) {
                ayni::foundation::store_le64(record.body.data() + tail + 24U,
                                             snapshot.metadata.covers_commit_seq + 1U);
            } else {
                ayni::foundation::store_le64(record.body.data() + tail, 0);
            }
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << fault;
    }
}

std::vector<StoredBatch> revoked_log(bool cleanup) {
    ReplayNode source;
    source.run();
    const auto root = source.create(1, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
    EXPECT_EQ(root.status, AYNI_OK);
    source.store(root.result.commit_seq);
    const auto child = source.create(2, 11, 19, root.result.object.id);
    EXPECT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    const auto revoked = source.revoke(3, root.result.object.id);
    EXPECT_EQ(revoked.status, AYNI_OK);
    EXPECT_EQ(revoked.result.phase, AYNI_INTENT_APPLIED);
    source.store(revoked.result.commit_seq);
    if (cleanup) {
        for (unsigned round = 0; round < 100; ++round) {
            const auto info = source.grant(root.result.object.id);
            if (info.cleanup_after_grant_id == info.cleanup_through_grant_id) {
                source.store(info.cleanup_commit_seq);
                break;
            }
            source.node.step(1);
            source.node.sim().complete_storage();
            source.node.sim().advance_us(1000000);
        }
        EXPECT_EQ(source.grant(child.result.object.id).lifecycle, AYNI_GRANT_REVOKED);
        const auto info = source.grant(root.result.object.id);
        EXPECT_EQ(info.cleanup_after_grant_id, info.cleanup_through_grant_id);
    }
    return source.node.sim().log();
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, root_record_and_exact_mapping_require_each_other_in_one_batch) {
    const auto original = revoked_log(false);
    for (const uint16_t removed :
         {ayni::authority::grant_revocation_record_type, ayni::kernel::command_record_type}) {
        bool edited = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            const bool revoke =
                std::any_of(batch.records.begin(), batch.records.end(), [](const Record &record) {
                    return record.header.type == ayni::authority::grant_revocation_record_type;
                });
            if (!revoke) {
                return;
            }
            batch.records.erase(std::remove_if(batch.records.begin(), batch.records.end(),
                                               [removed](const Record &record) {
                                                   return record.header.type == removed;
                                               }),
                                batch.records.end());
            edited = true;
        });
        ASSERT_TRUE(edited);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, duplicate_root_record_and_mismatched_effect_are_corrupt) {
    const auto original = revoked_log(false);
    for (unsigned fault = 0; fault < 2; ++fault) {
        bool edited = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            for (auto &record : batch.records) {
                if (record.header.type != ayni::authority::grant_revocation_record_type) {
                    continue;
                }
                if (fault == 0U) {
                    batch.records.push_back(record);
                } else {
                    // Syntactically valid NO_CHANGE cannot describe an ACTIVE target.
                    ayni::foundation::store_le32(record.body.data() + 12,
                                                 AYNI_INTENT_EFFECT_NO_CHANGE);
                    ayni::foundation::store_le64(record.body.data() + 24, 0);
                }
                edited = true;
                break;
            }
        });
        ASSERT_TRUE(edited);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, mapping_owner_and_object_must_match_the_accepted_root_record) {
    const auto original = revoked_log(false);
    for (unsigned fault = 0; fault < 2; ++fault) {
        bool edited = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            for (auto &record : batch.records) {
                if (record.header.type != ayni::kernel::command_record_type ||
                    record.header.version != ayni::kernel::revoke_command_record_version) {
                    continue;
                }
                CommandMapping mapping{};
                uint64_t floor = 0;
                ayni_bytes_v1 canonical{};
                ASSERT_TRUE(ayni::kernel::read_command_record(
                    record.header.version, record.body.data(), record.header.length, mapping, floor,
                    canonical));
                std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
                if (fault == 0U) {
                    ++mapping.principal_id;
                } else {
                    ++mapping.object.id;
                    ayni::foundation::store_le64(bytes.data() + 8, mapping.object.id);
                    mapping.digest = ayni::kernel::authority_command_digest(
                        {bytes.data(), static_cast<uint32_t>(bytes.size())});
                }
                ASSERT_TRUE(ayni::kernel::write_command_record(
                    mapping, floor, {bytes.data(), static_cast<uint32_t>(bytes.size())},
                    record.body.data(), record.header.length));
                edited = true;
            }
        });
        ASSERT_TRUE(edited);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, descendant_state_and_progress_are_an_atomic_pair) {
    const auto original = revoked_log(true);
    for (unsigned fault = 0; fault < 3; ++fault) {
        bool edited = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            const bool child_state =
                std::any_of(batch.records.begin(), batch.records.end(), [](const Record &record) {
                    return record.header.type == ayni::authority::grant_state_record_type &&
                           record.header.version ==
                               ayni::authority::grant_revoked_state_record_version;
                });
            if (!child_state) {
                return;
            }
            if (fault == 2U) {
                std::reverse(batch.records.begin(), batch.records.end());
            } else {
                const uint16_t removed =
                    fault == 0U ? ayni::authority::grant_state_record_type
                                : ayni::authority::grant_revocation_progress_record_type;
                batch.records.erase(std::remove_if(batch.records.begin(), batch.records.end(),
                                                   [removed](const Record &record) {
                                                       return record.header.type == removed;
                                                   }),
                                    batch.records.end());
            }
            edited = true;
        });
        ASSERT_TRUE(edited);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, progress_cannot_skip_an_active_descendant_or_change_its_watermark) {
    const auto original = revoked_log(true);
    for (const unsigned offset : {8U, 16U}) {
        bool edited = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            for (auto &record : batch.records) {
                if (record.header.type != ayni::authority::grant_revocation_progress_record_type) {
                    continue;
                }
                const auto value = ayni::foundation::load_le64(record.body.data() + offset);
                ayni::foundation::store_le64(record.body.data() + offset, value + 1U);
                edited = true;
                break;
            }
        });
        ASSERT_TRUE(edited);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-015/016 / ADR-0031 decision 7: every authority record must be REQUIRED.
TEST(GrantRevokeReplay, revoke_and_cleanup_headers_require_required_even_for_future_versions) {
    const auto original = revoked_log(true);
    for (const uint16_t type :
         {ayni::authority::grant_revocation_record_type,
          ayni::authority::grant_revocation_progress_record_type,
          ayni::authority::grant_state_record_type, ayni::kernel::command_record_type}) {
        for (const bool future : {false, true}) {
            bool edited = false;
            const auto log = edited_log(original, [&](Batch &batch) {
                for (auto &record : batch.records) {
                    if (record.header.type != type ||
                        (type == ayni::kernel::command_record_type &&
                         record.header.version != 3U) ||
                        (type == ayni::authority::grant_state_record_type &&
                         record.header.version != 2U)) {
                        continue;
                    }
                    record.header.flags = 0;
                    if (future) {
                        record.header.version = 99;
                    }
                    edited = true;
                }
            });
            ASSERT_TRUE(edited);
            expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
        }
    }
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshots_write_grant_v2_and_read_legacy_v1_without_inventing_revocations) {
    ReplayNode source;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    auto snapshot = snapshot_of(source);
    bool converted = false;
    for (auto &record : snapshot.records) {
        if (record.header.type != ayni::authority::grant_record_type) {
            continue;
        }
        ASSERT_EQ(record.header.version, 2U);
        ASSERT_GE(record.body.size(), 80U);
        record.body.resize(record.body.size() - 40U);
        record.header.length = static_cast<uint32_t>(record.body.size());
        record.header.version = 1;
        converted = true;
    }
    ASSERT_TRUE(converted);
    EXPECT_EQ(import_snapshot(snapshot,
                              [&](ayni::kernel::WorkModel &model) {
                                  ASSERT_EQ(model.grants.size(), 1U);
                                  EXPECT_EQ(model.grants.at(0).lifecycle, AYNI_GRANT_ACTIVE);
                                  EXPECT_EQ(model.grants.at(0).revoked_by_root_id, 0U);
                                  EXPECT_EQ(model.grants.at(0).cleanup_stamp.commit_seq, 0U);
                              }),
              BatchCheck::ok);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_cleanup_stamp_has_independent_transition_and_commit_bounds) {
    ReplayNode source;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto revoked = source.revoke(2, grant.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    const auto original = snapshot_of(source);
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    for (unsigned fault = 0; fault < 2; ++fault) {
        auto snapshot = original;
        bool edited = false;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::authority::grant_record_type) {
                continue;
            }
            const auto base = record.body.size() - 80U;
            ayni::foundation::store_le64(record.body.data() + base + (fault == 0U ? 64U : 72U),
                                         fault == 0U ? snapshot.metadata.max_transition_id + 1U
                                                     : snapshot.metadata.covers_commit_seq + 1U);
            edited = true;
        }
        ASSERT_TRUE(edited);
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << fault;
    }
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_revoke_effect_must_agree_with_the_retained_target_history) {
    ReplayNode source;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto revoked = source.revoke(2, grant.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    const auto original = snapshot_of(source);
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    for (const auto effect : {AYNI_INTENT_EFFECT_NO_CHANGE, AYNI_INTENT_EFFECT_TARGET_GONE}) {
        auto snapshot = original;
        bool edited = false;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::kernel::command_record_type ||
                record.header.version != 3U) {
                continue;
            }
            CommandMapping mapping{};
            uint64_t floor = 0;
            ayni_bytes_v1 canonical{};
            const auto base = record.header.length - 8U;
            ASSERT_TRUE(ayni::kernel::read_command_record(3, record.body.data(), base, mapping,
                                                          floor, canonical));
            mapping.effect = effect;
            mapping.object.revision = effect == AYNI_INTENT_EFFECT_TARGET_GONE ? 0U : 2U;
            std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
            ASSERT_TRUE(ayni::kernel::write_command_record(
                mapping, floor, {bytes.data(), static_cast<uint32_t>(bytes.size())},
                record.body.data(), base));
            edited = true;
        }
        ASSERT_TRUE(edited);
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << effect;
        // Missing targets are legitimate retained receipts after safe reclaim
        // or accepted volatile creation lost during reload.
        snapshot.records.erase(std::remove_if(snapshot.records.begin(), snapshot.records.end(),
                                              [](const Record &record) {
                                                  return record.header.type ==
                                                         ayni::authority::grant_record_type;
                                              }),
                               snapshot.records.end());
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::ok) << effect;
    }
    const auto no_change = source.revoke(3, grant.result.object.id, 2);
    ASSERT_EQ(no_change.status, AYNI_OK);
    ASSERT_EQ(no_change.result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    source.store(no_change.result.commit_seq);
    EXPECT_EQ(import_snapshot(snapshot_of(source)), BatchCheck::ok);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_revoke_mapping_validates_binding_sequence_and_kernel_id) {
    ReplayNode source;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto revoked = source.revoke(2, grant.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    const auto original = snapshot_of(source);
    for (unsigned fault = 0; fault < 4; ++fault) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::kernel::command_record_type ||
                record.header.version != 3U) {
                continue;
            }
            CommandMapping mapping{};
            uint64_t floor = 0;
            ayni_bytes_v1 canonical{};
            const auto base = record.header.length - 8U;
            ASSERT_TRUE(ayni::kernel::read_command_record(3, record.body.data(), base, mapping,
                                                          floor, canonical));
            std::vector<uint8_t> bytes(canonical.data, canonical.data + canonical.size);
            if (fault == 0U) {
                ++mapping.principal_id;
            }
            if (fault >= 2U) {
                mapping.object.id =
                    fault == 2U ? 17U : ((snapshot.metadata.boot_epoch + 1U) << 32U) | 1U;
                ayni::foundation::store_le64(bytes.data() + 8, mapping.object.id);
                mapping.digest = ayni::kernel::authority_command_digest(
                    {bytes.data(), static_cast<uint32_t>(bytes.size())});
            }
            ASSERT_TRUE(ayni::kernel::write_command_record(
                mapping, floor, {bytes.data(), static_cast<uint32_t>(bytes.size())},
                record.body.data(), base));
            if (fault == 1U) {
                ayni::foundation::store_le64(record.body.data() + base, 1);
            }
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << fault;
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, a_foreign_mapping_principal_cannot_adopt_a_valid_grants_owner_in_the_journal) {
    const auto original = complete_log();
    {
        ReplayNode normal;
        normal.node.sim().log() = original;
        normal.node.create_and_start();
        normal.node.recover();
        ASSERT_EQ(normal.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        ASSERT_EQ(KernelTestProbe::work(normal.node.kernel()).grants.size(), 1U);
        EXPECT_EQ(KernelTestProbe::work(normal.node.kernel()).grants.at(0).owner_principal_id,
                  owner_id);
    }
    bool changed = false;
    const auto damaged = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type == ayni::kernel::command_record_type &&
                record.header.version == ayni::kernel::exact_command_record_version) {
                ASSERT_EQ(ayni::foundation::load_le64(record.body.data()), owner_id);
                // principal is outside the canonical request/digest. All
                // Grant, binding, result and request bytes remain original;
                // edited_log recomputes the complete batch framing and CRC.
                ayni::foundation::store_le64(record.body.data(), owner_id + 1U);
                changed = true;
            }
        }
    });
    ASSERT_TRUE(changed);
    ReplayNode restored;
    restored.node.sim().log() = damaged;
    const auto before = restored.node.sim().log();
    restored.node.create_and_start();
    restored.node.recover();
    EXPECT_EQ(restored.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(restored.node.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    EXPECT_EQ(KernelTestProbe::work(restored.node.kernel()).grants.size(), 0U);
    ASSERT_EQ(restored.node.sim().log().size(), before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(restored.node.sim().log()[index].bytes, before[index].bytes);
    }
}

// C-STORE-015 / ADR-0030 decision 8: recovery checks complete authority facts and pairings.
TEST(GrantReplay, expired_snapshot_bounds_each_terminal_stamp_component_independently) {
    ReplayNode source;
    source.run();
    const auto created = source.create(1, 10, 20, 0, 10000000);
    ASSERT_EQ(created.status, AYNI_OK);
    source.store(created.result.commit_seq);
    source.expire(created.result.object.id);
    ASSERT_EQ(source.grant(created.result.object.id).lifecycle, AYNI_GRANT_EXPIRED);
    ASSERT_EQ(source.grant(created.result.object.id).revision, 2U);
    const auto original = snapshot_of(source);
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);

    // An EXPIRED record has a distinct terminal stamp. Keep the creation stamp
    // and the other terminal component valid so neither masks the bound checked.
    constexpr uint32_t terminal_transition_offset = 16;
    constexpr uint32_t terminal_commit_offset = 24;
    for (const bool corrupt_commit : {false, true}) {
        auto snapshot = original;
        bool changed = false;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::authority::grant_record_type) {
                continue;
            }
            ayni::authority::GrantSnapshotRecord decoded{};
            ASSERT_TRUE(ayni::authority::read_grant_snapshot_record(
                record.body.data(), record.header.length, decoded, record.header.version));
            ASSERT_EQ(decoded.lifecycle, AYNI_GRANT_EXPIRED);
            ASSERT_EQ(decoded.revision, 2U);
            ASSERT_GT(decoded.stamp.transition_id, decoded.created_stamp.transition_id);
            ASSERT_GT(decoded.stamp.commit_seq, decoded.created_stamp.commit_seq);
            ASSERT_LE(decoded.stamp.transition_id, snapshot.metadata.max_transition_id);
            ASSERT_LE(decoded.stamp.commit_seq, snapshot.metadata.covers_commit_seq);
            const auto tail =
                record.body.size() - (record.header.version == 1U
                                          ? ayni::authority::grant_snapshot_tail_size
                                          : ayni::authority::grant_revocation_snapshot_tail_size);
            const uint32_t offset =
                corrupt_commit ? terminal_commit_offset : terminal_transition_offset;
            const uint64_t invalid = corrupt_commit ? snapshot.metadata.covers_commit_seq + 1U
                                                    : snapshot.metadata.max_transition_id + 1U;
            ayni::foundation::store_le64(record.body.data() + tail + offset, invalid);
            changed = true;
        }
        ASSERT_TRUE(changed);
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << corrupt_commit;
    }
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_cleanup_watermarks_cannot_name_a_future_boot) {
    ReplayNode source;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto revoked = source.revoke(2, grant.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    const auto original = snapshot_of(source);
    ASSERT_EQ(import_snapshot(original), BatchCheck::ok);
    for (bool complete : {false, true}) {
        auto snapshot = original;
        for (auto &record : snapshot.records) {
            if (record.header.type != ayni::authority::grant_record_type) {
                continue;
            }
            const auto tail = record.body.size() - 80U;
            const uint64_t future = ((snapshot.metadata.boot_epoch + 1U) << 32U) | 1U;
            ayni::foundation::store_le64(record.body.data() + tail + 56U, future);
            if (complete) {
                ayni::foundation::store_le64(record.body.data() + tail + 48U, future);
            }
        }
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << complete;
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, accepted_mapping_cannot_claim_an_unreachable_original_revision) {
    const auto original = revoked_log(false);
    bool edited = false;
    const auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type != ayni::kernel::command_record_type ||
                record.header.version != ayni::kernel::revoke_command_record_version) {
                continue;
            }
            // Independent v3 framing oracle: common mapping prefix digest at
            // byte16, canonical body after the64-byte prefix, expected at+4.
            ASSERT_EQ(record.body.size(), 80U);
            ayni::foundation::store_le32(record.body.data() + 68U, 999);
            const ayni_bytes_v1 request{record.body.data() + 64U, 16};
            ayni::kernel::RevokeCommandRecord raw{};
            ASSERT_TRUE(ayni::kernel::read_revoke_command(request, raw));
            EXPECT_EQ(raw.expected_revision, 999U); // request syntax remains valid
            ayni::foundation::store_le64(record.body.data() + 16U,
                                         ayni::kernel::authority_command_digest(request));
            edited = true;
        }
    });
    ASSERT_TRUE(edited);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_root_frontier_cannot_exclude_a_retained_descendant) {
    for (const bool expired_child : {false, true}) {
        ReplayNode source;
        source.run();
        const auto root = source.create(1, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
        ASSERT_EQ(root.status, AYNI_OK);
        source.store(root.result.commit_seq);
        const auto child =
            source.create(2, 11, 19, root.result.object.id, expired_child ? 10000000U : 0U);
        ASSERT_EQ(child.status, AYNI_OK);
        source.store(child.result.commit_seq);
        if (expired_child) {
            source.expire(child.result.object.id);
        }
        const auto revoked = source.revoke(3, root.result.object.id);
        ASSERT_EQ(revoked.status, AYNI_OK);
        source.store(revoked.result.commit_seq);
        auto snapshot = snapshot_of(source);
        ASSERT_EQ(import_snapshot(snapshot), BatchCheck::ok);
        bool changed = false;
        for (auto &record : snapshot.records) {
            if (record.header.type == ayni::authority::grant_record_type &&
                ayni::foundation::load_le64(record.body.data()) == root.result.object.id) {
                const auto tail = record.body.size() - 80U;
                // Preserve creation, terminal and progress stamps, the exact
                // command, and the descendant. Only shrink the fixed frontier.
                ayni::foundation::store_le64(record.body.data() + tail + 56U,
                                             root.result.object.id);
                changed = true;
            }
        }
        ASSERT_TRUE(changed);
        EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt) << expired_child;
    }
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, record_versions_are_specific_to_journal_and_snapshot_contexts) {
    using ayni::kernel::WorkRecordContext;
    using ayni::kernel::WorkReplay;
    for (const uint16_t type :
         {ayni::authority::grant_state_record_type, ayni::authority::grant_revocation_record_type,
          ayni::authority::grant_revocation_progress_record_type}) {
        const RecordHeader known{type, 1, record_required, 0};
        EXPECT_EQ(WorkReplay::classify_header(known), RecordUse::applied);
        EXPECT_EQ(WorkReplay::classify_header(known, WorkRecordContext::snapshot),
                  RecordUse::unknown);
        const RecordHeader future{type, 99, record_required, 0};
        EXPECT_EQ(WorkReplay::classify_header(future), RecordUse::unknown);
        EXPECT_EQ(WorkReplay::classify_header({type, 99, 0, 0}), RecordUse::corrupt);
    }
    const RecordHeader snapshot_grant{ayni::authority::grant_record_type, 2, record_required, 0};
    // ADR-0037: journal v2 is now a different, Goal-bound creation body.
    // Header classification cannot infer snapshot semantics without context.
    EXPECT_EQ(WorkReplay::classify_header(snapshot_grant), RecordUse::applied);
    const RecordHeader bound_snapshot{ayni::authority::grant_record_type,
                                      ayni::authority::goal_grant_snapshot_record_version,
                                      record_required, 0};
    EXPECT_EQ(WorkReplay::classify_header(bound_snapshot), RecordUse::unknown);
    EXPECT_EQ(WorkReplay::classify_header(bound_snapshot, WorkRecordContext::snapshot),
              RecordUse::applied);
    EXPECT_EQ(WorkReplay::classify_header(snapshot_grant, WorkRecordContext::snapshot),
              RecordUse::applied);
    EXPECT_EQ(
        WorkReplay::classify_header({ayni::authority::grant_record_type, 99, record_required, 0},
                                    WorkRecordContext::snapshot),
        RecordUse::unknown);
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, root_and_descendant_records_reject_truncated_bodies_inside_valid_batches) {
    const auto original = revoked_log(true);
    for (const uint16_t type : {ayni::authority::grant_revocation_record_type,
                                ayni::authority::grant_revocation_progress_record_type,
                                ayni::authority::grant_state_record_type}) {
        bool changed = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            for (auto &record : batch.records) {
                if (record.header.type != type) {
                    continue;
                }
                record.body.pop_back();
                record.header.length = static_cast<uint32_t>(record.body.size());
                changed = true;
            }
        });
        ASSERT_TRUE(changed);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, duplicate_pending_root_or_child_cannot_claim_one_transition) {
    const auto original = revoked_log(true);
    for (const uint16_t type :
         {ayni::authority::grant_revocation_record_type, ayni::authority::grant_state_record_type,
          ayni::authority::grant_revocation_progress_record_type}) {
        bool changed = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            const auto found =
                std::find_if(batch.records.begin(), batch.records.end(),
                             [type](const Record &record) { return record.header.type == type; });
            if (found == batch.records.end()) {
                return;
            }
            const auto index = static_cast<std::size_t>(found - batch.records.begin());
            const Record duplicate = *found;
            batch.records.insert(batch.records.begin() + static_cast<std::ptrdiff_t>(index),
                                 duplicate);
            changed = true;
        });
        ASSERT_TRUE(changed);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, descendant_and_progress_must_name_the_same_root_and_cursor) {
    const auto original = revoked_log(true);
    for (const unsigned offset : {0U, 8U}) {
        bool changed = false;
        const auto log = edited_log(original, [&](Batch &batch) {
            const auto child =
                std::find_if(batch.records.begin(), batch.records.end(), [](const Record &record) {
                    return record.header.type == ayni::authority::grant_state_record_type;
                });
            if (child == batch.records.end()) {
                return;
            }
            for (auto &record : batch.records) {
                if (record.header.type != ayni::authority::grant_revocation_progress_record_type) {
                    continue;
                }
                const uint64_t other = offset == 0U
                                           ? ayni::foundation::load_le64(child->body.data())
                                           : ayni::foundation::load_le64(child->body.data() + 16U);
                ayni::foundation::store_le64(record.body.data() + offset, other);
                changed = true;
            }
        });
        ASSERT_TRUE(changed);
        expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, matching_syntax_still_requires_the_root_and_mapping_effects_to_agree) {
    const auto original = revoked_log(false);
    bool changed = false;
    const auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type != ayni::kernel::command_record_type ||
                record.header.version != 3U) {
                continue;
            }
            CommandMapping mapping{};
            uint64_t floor = 0;
            ayni_bytes_v1 canonical{};
            ASSERT_TRUE(ayni::kernel::read_command_record(
                3, record.body.data(), record.header.length, mapping, floor, canonical));
            mapping.effect = AYNI_INTENT_EFFECT_NO_CHANGE;
            std::vector<uint8_t> request(canonical.data, canonical.data + canonical.size);
            ASSERT_TRUE(ayni::kernel::write_command_record(
                mapping, floor, {request.data(), static_cast<uint32_t>(request.size())},
                record.body.data(), record.header.length));
            changed = true;
        }
    });
    ASSERT_TRUE(changed);
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, a_later_boot_record_cannot_legitimize_an_earlier_future_target_gone) {
    const auto original = revoked_log(false);
    bool changed = false;
    auto log = edited_log(original, [&](Batch &batch) {
        for (auto &record : batch.records) {
            if (record.header.type == ayni::authority::grant_revocation_record_type) {
                const auto old = ayni::foundation::load_le64(record.body.data());
                const uint64_t target = old + (UINT64_C(1) << 32U);
                ayni::foundation::store_le64(record.body.data(), target);
                ayni::foundation::store_le32(record.body.data() + 8U, 0);
                ayni::foundation::store_le32(record.body.data() + 12U,
                                             AYNI_INTENT_EFFECT_TARGET_GONE);
                ayni::foundation::store_le64(record.body.data() + 24U, 0);
                changed = true;
            } else if (record.header.type == ayni::kernel::command_record_type &&
                       record.header.version == 3U) {
                CommandMapping mapping{};
                uint64_t floor = 0;
                ayni_bytes_v1 canonical{};
                ASSERT_TRUE(ayni::kernel::read_command_record(
                    3, record.body.data(), record.header.length, mapping, floor, canonical));
                mapping.object.id += UINT64_C(1) << 32U;
                mapping.object.revision = 0;
                mapping.effect = AYNI_INTENT_EFFECT_TARGET_GONE;
                std::vector<uint8_t> request(canonical.data, canonical.data + canonical.size);
                ayni::foundation::store_le64(request.data() + 8U, mapping.object.id);
                const ayni_bytes_v1 bytes{request.data(), static_cast<uint32_t>(request.size())};
                mapping.digest = ayni::kernel::authority_command_digest(bytes);
                ASSERT_TRUE(ayni::kernel::write_command_record(
                    mapping, floor, bytes, record.body.data(), record.header.length));
            }
        }
    });
    ASSERT_TRUE(changed);
    const auto last = decode(log.back());
    Batch later{{last.identity.writer_epoch, last.identity.commit_seq + 1U,
                 last.identity.transition_id + 1U},
                last.schema,
                {}};
    Record boot{{ayni::kernel::boot_epoch_record_type, 1, record_required, 8},
                std::vector<uint8_t>(8)};
    ayni::foundation::store_le64(boot.body.data(), 2);
    later.records.push_back(boot);
    log.push_back(encode(later));
    expect_restricted(log, AYNI_RESTRICTION_LOG_CORRUPT);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_changed_mapping_cannot_move_after_its_terminal_transition) {
    ReplayNode source;
    source.node.config().command_capacity = 2;
    source.run();
    const auto grant = source.create(1);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto revoked = source.revoke(2, grant.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 3, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    auto snapshot = snapshot_of(source);
    ASSERT_EQ(import_snapshot(snapshot), BatchCheck::ok);
    ASSERT_GT(paused.result.commit_seq, revoked.result.commit_seq + 1U);
    for (auto &record : snapshot.records) {
        if (record.header.type == ayni::kernel::command_record_type &&
            record.header.version == 3U) {
            ayni::foundation::store_le64(record.body.data() + record.body.size() - 8U,
                                         revoked.result.commit_seq + 1U);
        }
    }
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay, snapshot_no_change_cannot_precede_the_retained_targets_terminal_stamp) {
    ReplayNode source;
    source.node.config().command_capacity = 1;
    source.run();
    const auto grant = source.create(1, 10, 20, 0, 10000000);
    ASSERT_EQ(grant.status, AYNI_OK);
    source.store(grant.result.commit_seq);
    const auto paused = source.change_instance(AYNI_INTENT_INSTANCE_PAUSE, 2, 1);
    ASSERT_EQ(paused.status, AYNI_OK);
    source.store(paused.result.commit_seq);
    source.expire(grant.result.object.id);
    const auto revoked = source.revoke(3, grant.result.object.id, 2);
    ASSERT_EQ(revoked.status, AYNI_OK);
    ASSERT_EQ(revoked.result.effect, AYNI_INTENT_EFFECT_NO_CHANGE);
    source.store(revoked.result.commit_seq);
    auto snapshot = snapshot_of(source);
    ASSERT_EQ(import_snapshot(snapshot), BatchCheck::ok);
    for (auto &record : snapshot.records) {
        if (record.header.type == ayni::kernel::command_record_type &&
            record.header.version == 3U) {
            ayni::foundation::store_le64(record.body.data() + record.body.size() - 8U,
                                         paused.result.commit_seq);
        }
    }
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt);
}

// C-STORE-017 / ADR-0031 decision 7: snapshots preserve revocation history and versions.
TEST(GrantRevokeReplay,
     snapshot_changed_receipt_cannot_claim_a_descendant_cleaned_by_another_root) {
    ReplayNode source;
    source.run();
    const auto root = source.create(1, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
    ASSERT_EQ(root.status, AYNI_OK);
    source.store(root.result.commit_seq);
    const auto child = source.create(2, 11, 19, root.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    const auto revoked = source.revoke(3, root.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    source.store(revoked.result.commit_seq);
    for (unsigned round = 0;
         round < 20 && source.grant(child.result.object.id).lifecycle != AYNI_GRANT_REVOKED;
         ++round) {
        source.node.step(1);
        source.node.sim().complete_storage();
        source.node.sim().advance_us(1000000);
    }
    const auto cleaned = source.grant(child.result.object.id);
    ASSERT_EQ(cleaned.lifecycle, AYNI_GRANT_REVOKED);
    ASSERT_EQ(cleaned.revoked_by_root_id, root.result.object.id);
    source.store(cleaned.commit_seq);
    auto snapshot = snapshot_of(source);
    ASSERT_EQ(import_snapshot(snapshot), BatchCheck::ok);
    for (auto &record : snapshot.records) {
        if (record.header.type != ayni::kernel::command_record_type ||
            record.header.version != 3U) {
            continue;
        }
        CommandMapping mapping{};
        uint64_t floor = 0;
        ayni_bytes_v1 canonical{};
        const auto base = record.header.length - 8U;
        ASSERT_TRUE(ayni::kernel::read_command_record(3, record.body.data(), base, mapping, floor,
                                                      canonical));
        mapping.object.id = child.result.object.id;
        std::vector<uint8_t> request(canonical.data, canonical.data + canonical.size);
        ayni::foundation::store_le64(request.data() + 8U, mapping.object.id);
        const ayni_bytes_v1 bytes{request.data(), static_cast<uint32_t>(request.size())};
        mapping.digest = ayni::kernel::authority_command_digest(bytes);
        ASSERT_TRUE(
            ayni::kernel::write_command_record(mapping, floor, bytes, record.body.data(), base));
        ayni::foundation::store_le64(record.body.data() + base, cleaned.commit_seq);
    }
    EXPECT_EQ(import_snapshot(snapshot), BatchCheck::corrupt);
}

void check_small_transition_buffers(const ayni::kernel::WorkModel &model) {
    const auto stamp = model.transitions.latest();
    const BatchIdentity identity{1, stamp.commit_seq, stamp.transition_id};
    std::array<uint8_t, 4096> full{};
    BatchWriter normal(full.data(), full.size(), identity);
    const auto required = ayni::kernel::encode_transition(model, stamp, normal);
    ASSERT_GT(required, batch_header_size + commit_marker_size);
    for (const auto capacity :
         {batch_header_size + commit_marker_size, batch_header_size + commit_marker_size + 16U,
          batch_header_size + commit_marker_size + 48U, required - 1U}) {
        std::vector<uint8_t> bytes(capacity + 8U, 0xA5);
        BatchWriter small(bytes.data(), capacity, identity);
        EXPECT_EQ(ayni::kernel::encode_transition(model, stamp, small), 0U);
        EXPECT_TRUE(std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(capacity), bytes.end(),
                                [](uint8_t value) { return value == 0xA5; }));
    }
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay, journal_root_and_progress_encoders_refuse_short_buffers_without_overrun) {
    ReplayNode source;
    source.run();
    const auto root = source.create(1, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
    ASSERT_EQ(root.status, AYNI_OK);
    source.store(root.result.commit_seq);
    const auto child = source.create(2, 11, 19, root.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    const auto revoked = source.revoke(3, root.result.object.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    check_small_transition_buffers(KernelTestProbe::work(source.node.kernel()));
    for (unsigned round = 0;
         round < 20 && source.grant(child.result.object.id).lifecycle != AYNI_GRANT_REVOKED;
         ++round) {
        source.node.step(1);
        source.node.sim().complete_storage();
        source.node.sim().advance_us(1000000);
    }
    ASSERT_EQ(source.grant(child.result.object.id).lifecycle, AYNI_GRANT_REVOKED);
    check_small_transition_buffers(KernelTestProbe::work(source.node.kernel()));
}

// C-STORE-016 / ADR-0031 decision 6: revocation facts and partners commit atomically.
TEST(GrantRevokeReplay,
     a_later_cursor_cannot_repair_a_child_state_paired_with_another_roots_progress) {
    ReplayNode source;
    source.node.config().grant_capacity = 3;
    source.run();
    const auto root_a = source.create(1, 10, 20, 0, 0, AYNI_GRANT_ALLOW_DELEGATION);
    ASSERT_EQ(root_a.status, AYNI_OK);
    source.store(root_a.result.commit_seq);
    const auto root_b = source.create(2);
    ASSERT_EQ(root_b.status, AYNI_OK);
    source.store(root_b.result.commit_seq);
    const auto child = source.create(3, 11, 19, root_a.result.object.id);
    ASSERT_EQ(child.status, AYNI_OK);
    source.store(child.result.commit_seq);
    const uint64_t a = root_a.result.object.id;
    const uint64_t b = root_b.result.object.id;
    const uint64_t x = child.result.object.id;
    ASSERT_LT(a, b);
    ASSERT_LT(b, x);

    const auto required = [](uint16_t type, uint16_t version, uint32_t size) {
        return Record{{type, version, record_required, size}, std::vector<uint8_t>(size)};
    };
    const auto revoke_pair = [&](uint64_t target, uint64_t command) {
        const ayni::authority::GrantTable::RevokePlan plan{target, 2, AYNI_INTENT_EFFECT_CHANGED, 0,
                                                           x};
        auto revoked = required(ayni::authority::grant_revocation_record_type,
                                ayni::authority::grant_revocation_record_version,
                                ayni::authority::grant_revocation_record_size);
        ayni::authority::write_grant_revocation_record(plan, revoked.body.data());
        const ayni_grant_revoke_v1 request{sizeof(request), 0, target};
        std::array<uint8_t, ayni::kernel::revoke_command_size> canonical{};
        uint32_t encoded_size = 0;
        EXPECT_EQ(ayni::kernel::encode_revoke_command({1, &request}, canonical.data(),
                                                      ayni::kernel::revoke_command_size,
                                                      encoded_size),
                  AYNI_OK);
        EXPECT_EQ(encoded_size, ayni::kernel::revoke_command_size);
        const ayni_bytes_v1 identity{canonical.data(), ayni::kernel::revoke_command_size};
        const CommandMapping mapping{owner_id,
                                     command,
                                     ayni::kernel::authority_command_digest(identity),
                                     0,
                                     AYNI_INTENT_GRANT_REVOKE,
                                     {0, target, 2, AYNI_OBJECT_GRANT},
                                     ayni::kernel::revoke_command_size,
                                     AYNI_INTENT_EFFECT_CHANGED};
        auto mapped = required(
            ayni::kernel::command_record_type, ayni::kernel::revoke_command_record_version,
            ayni::kernel::revoke_command_record_fixed_size + ayni::kernel::revoke_command_size);
        EXPECT_TRUE(ayni::kernel::write_command_record(mapping, 0, identity, mapped.body.data(),
                                                       mapped.header.length));
        return std::vector<Record>{revoked, mapped};
    };
    const auto progress = [&](uint64_t root, uint64_t after) {
        const ayni::authority::GrantTable::CleanupPlan plan{root, after, x, 0};
        auto record = required(ayni::authority::grant_revocation_progress_record_type,
                               ayni::authority::grant_revocation_progress_record_version,
                               ayni::authority::grant_revocation_progress_record_size);
        ayni::authority::write_grant_revocation_progress_record(plan, record.body.data());
        return record;
    };
    const auto append = [](std::vector<StoredBatch> &log, const std::vector<Record> &records) {
        const auto previous = decode(log.back());
        Batch next{previous.identity, previous.schema, records};
        ++next.identity.commit_seq;
        ++next.identity.transition_id;
        log.push_back(encode(next)); // Every synthetic history retains a valid batch CRC.
    };
    auto prefix = source.node.sim().log();
    append(prefix, revoke_pair(a, 4));
    append(prefix, revoke_pair(b, 5));
    for (const auto root : {a, b}) {
        append(prefix, {progress(root, a)});
        append(prefix, {progress(root, b)});
    }
    // White-box fixture: copy immutable facts for the synthetic state codec;
    // the live Grant table is not modified.
    uint32_t child_index = 0;
    ASSERT_TRUE(KernelTestProbe::work(source.node.kernel()).grants.find(x, child_index));
    auto child_fact = KernelTestProbe::work(source.node.kernel()).grants.at(child_index);
    child_fact.lifecycle = AYNI_GRANT_REVOKED;
    child_fact.revision = 2;
    child_fact.revoked_by_root_id = a;
    auto child_state = required(ayni::authority::grant_state_record_type,
                                ayni::authority::grant_revoked_state_record_version,
                                ayni::authority::grant_revoked_state_record_size);
    ayni::authority::write_grant_state_record(child_fact, child_state.body.data());

    // Both histories finish with A/B cursors at X. In the bad history S
    // pairs child X/by A with B's progress; T later advances A without a
    // state record, so final cross-table consistency alone cannot detect S.
    auto normal = prefix;
    append(normal, {child_state, progress(a, x)});
    append(normal, {progress(b, x)});
    ReplayNode valid;
    valid.node.config().grant_capacity = 3;
    valid.node.sim().log() = normal;
    valid.node.create_and_start();
    valid.node.recover();
    ASSERT_EQ(valid.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(valid.grant(a).cleanup_after_grant_id, x);
    EXPECT_EQ(valid.grant(b).cleanup_after_grant_id, x);
    EXPECT_EQ(valid.grant(x).revoked_by_root_id, a);

    auto damaged = prefix;
    append(damaged, {child_state, progress(b, x)});
    append(damaged, {progress(a, x)});
    ReplayNode restored;
    restored.node.config().grant_capacity = 3;
    restored.node.sim().log() = damaged;
    const auto before = restored.node.sim().log();
    restored.node.create_and_start();
    restored.node.recover();
    EXPECT_EQ(restored.node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(restored.node.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    // White-box: failed recovery must discard its partially restored facts.
    EXPECT_EQ(KernelTestProbe::work(restored.node.kernel()).grants.size(), 0U);
    ASSERT_EQ(restored.node.sim().log().size(), before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(restored.node.sim().log()[index].bytes, before[index].bytes);
    }
}

// White-box fixture: feed real stored domain records through WorkReplay's public
// record interface, pausing at a semantic boundary before the whole boot finishes.
// boot_epoch is the one the log's BOOT_EPOCH records name before the record.
struct DomainRecord {
    BatchIdentity batch;
    Record record;
    uint64_t boot_epoch;
};

std::vector<DomainRecord> domain_records(const std::vector<StoredBatch> &log) {
    std::vector<DomainRecord> result;
    uint64_t boot_epoch = 0;
    for (const auto &stored : log) {
        const auto batch = decode(stored);
        for (const auto &record : batch.records) {
            if (record.header.type == ayni::kernel::boot_epoch_record_type) {
                boot_epoch = ayni::foundation::load_le64(record.body.data());
            }
            if (ayni::kernel::WorkReplay::classify_header(record.header) == RecordUse::applied) {
                result.push_back({batch.identity, record, boot_epoch});
            }
        }
    }
    return result;
}

// C-STORE-016: an accepted descendant state remains incomplete until its progress arrives.
TEST(GrantRevokeReplay, a_descendant_state_keeps_replay_incomplete_until_its_progress) {
    const auto records = domain_records(revoked_log(true));
    ReplayNode restored;
    restored.node.create();
    auto &replay = KernelTestProbe::replay(restored.node.kernel());
    bool saw_child = false;
    for (const auto &item : records) {
        ASSERT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                      item.record.body.data(), item.boot_epoch),
                  RecordUse::applied);
        if (item.record.header.type == ayni::authority::grant_state_record_type &&
            item.record.header.version == ayni::authority::grant_revoked_state_record_version) {
            saw_child = true;
            EXPECT_FALSE(replay.complete());
        }
    }
    ASSERT_TRUE(saw_child);
    EXPECT_TRUE(replay.complete());
    EXPECT_TRUE(replay.consistent());
}

// C-STORE-015: one missing command cannot be hidden by another Grant's create record.
TEST(GrantReplay, a_second_create_cannot_replace_an_unmatched_create_in_the_same_batch) {
    ReplayNode source;
    source.run();
    for (const uint64_t command : {1U, 2U}) {
        const auto created = source.create(command);
        ASSERT_EQ(created.status, AYNI_OK);
        source.store(created.result.commit_seq);
    }
    const auto records = domain_records(source.node.sim().log());
    ReplayNode restored;
    restored.node.create();
    auto &replay = KernelTestProbe::replay(restored.node.kernel());
    BatchIdentity first_batch{};
    bool waiting = false;
    for (const auto &item : records) {
        if (waiting) {
            if (item.record.header.type == ayni::authority::grant_record_type) {
                EXPECT_EQ(
                    ayni::kernel::WorkReplay::on_record(&replay, first_batch, item.record.header,
                                                        item.record.body.data(), item.boot_epoch),
                    RecordUse::corrupt);
                return;
            }
            continue; // Withhold the first command to exercise the unmatched boundary.
        }
        ASSERT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                      item.record.body.data(), item.boot_epoch),
                  RecordUse::applied);
        if (item.record.header.type == ayni::authority::grant_record_type) {
            first_batch = item.batch;
            waiting = true;
            ASSERT_FALSE(replay.complete());
        }
    }
    FAIL() << "fixture did not contain two distinct root creations";
}

// C-STORE-015/016: cleanup progress cannot consume an unrelated create's missing command.
TEST(GrantRevokeReplay, valid_progress_cannot_interrupt_a_create_waiting_for_its_command) {
    const auto records = domain_records(revoked_log(true));
    ReplayNode restored;
    restored.node.config().grant_capacity = 3;
    restored.node.create();
    auto &replay = KernelTestProbe::replay(restored.node.kernel());
    uint64_t fresh_id = 0;
    for (const auto &item : records) {
        if (item.record.header.type == ayni::authority::grant_record_type) {
            fresh_id = std::max(fresh_id, ayni::foundation::load_le64(item.record.body.data()));
        }
    }
    ++fresh_id;
    Record another_create{};
    for (const auto &item : records) {
        if (item.record.header.type == ayni::authority::grant_record_type &&
            another_create.body.empty()) {
            another_create = item.record;
            // C-STORE-015: the create body's first u64 is its Grant identity.
            // The new root is beyond the original cleanup's frozen frontier.
            ayni::foundation::store_le64(another_create.body.data(), fresh_id);
        }
        if (item.record.header.type == ayni::authority::grant_revocation_progress_record_type) {
            ASSERT_FALSE(another_create.body.empty());
            ASSERT_EQ(
                ayni::kernel::WorkReplay::on_record(&replay, item.batch, another_create.header,
                                                    another_create.body.data(), item.boot_epoch),
                RecordUse::applied);
            ASSERT_FALSE(replay.complete());
            EXPECT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                          item.record.body.data(), item.boot_epoch),
                      RecordUse::corrupt);
            return;
        }
        ASSERT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                      item.record.body.data(), item.boot_epoch),
                  RecordUse::applied);
    }
    FAIL() << "fixture did not contain a valid standalone cleanup progress";
}

// ADR-0026/0035: discarding an incomplete prefix also forgets every waiting partner.
TEST(GrantReplay, discarding_an_unmatched_create_allows_a_clean_replay_from_the_start) {
    const auto records = domain_records(complete_log());
    ReplayNode restored;
    restored.node.create();
    auto &replay = KernelTestProbe::replay(restored.node.kernel());
    bool waiting = false;
    for (const auto &item : records) {
        ASSERT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                      item.record.body.data(), item.boot_epoch),
                  RecordUse::applied);
        if (item.record.header.type == ayni::authority::grant_record_type) {
            waiting = true;
            break;
        }
    }
    ASSERT_TRUE(waiting);
    ASSERT_FALSE(replay.complete());
    replay.discard();
    EXPECT_TRUE(replay.complete());
    for (const auto &item : records) {
        ASSERT_EQ(ayni::kernel::WorkReplay::on_record(&replay, item.batch, item.record.header,
                                                      item.record.body.data(), item.boot_epoch),
                  RecordUse::applied);
    }
    EXPECT_TRUE(replay.complete());
    EXPECT_TRUE(replay.consistent());
}

} // namespace
