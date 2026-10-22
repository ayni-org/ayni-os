// Faults below are complete, CRC-valid batches or framed snapshot records.
// They exercise semantic recovery checks rather than checksum rejection.
#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "durability/batch/batch_codec.h"
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"
#include "kernel/work_snapshot.h"
#include "work_node.h"

#include <ayni/authority.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

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
    EXPECT_EQ(restored.node.kernel()->work.grants.size(), 0U);
}

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

TEST(GrantReplay, a_foreign_mapping_principal_cannot_adopt_a_valid_grants_owner_in_the_journal) {
    const auto original = complete_log();
    {
        ReplayNode normal;
        normal.node.sim().log() = original;
        normal.node.create_and_start();
        normal.node.recover();
        ASSERT_EQ(normal.node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        ASSERT_EQ(normal.node.kernel()->work.grants.size(), 1U);
        EXPECT_EQ(normal.node.kernel()->work.grants.at(0).owner_principal_id, owner_id);
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
    EXPECT_EQ(restored.node.kernel()->work.grants.size(), 0U);
    ASSERT_EQ(restored.node.sim().log().size(), before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        EXPECT_EQ(restored.node.sim().log()[index].bytes, before[index].bytes);
    }
}

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
                            record.header.version =
                                static_cast<uint16_t>(record.header.version + (future ? 1U : 0U));
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
                    static_cast<uint16_t>((type == ayni::kernel::command_record_type ? 2U : 1U) +
                                          (future ? 1U : 0U)),
                    required ? record_required : 0U, 0};
                EXPECT_EQ(ayni::kernel::WorkReplay::classify_header(header),
                          !required ? RecordUse::corrupt
                          : future  ? RecordUse::unknown
                                    : RecordUse::applied);
            }
        }
    }
}

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

struct Snapshot {
    SnapshotMetadata metadata;
    std::vector<Record> records;
};

Snapshot snapshot_of(ReplayNode &source) {
    // White-box: snapshot module export/import owns the durable model here;
    // no authorization or recovery gate is bypassed in public fixtures.
    auto &model = source.node.kernel()->work;
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
    ayni::kernel::WorkSnapshotReader reader(restored.node.kernel()->work, metadata);
    for (const auto &record : snapshot.records) {
        const auto check = reader.apply(record.header, record.body.data());
        if (check != BatchCheck::ok) {
            return check;
        }
    }
    const auto check = reader.complete();
    if (check == BatchCheck::ok && verify) {
        verify(restored.node.kernel()->work);
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
    ASSERT_EQ(source.node.kernel()->work.grants.at(0).grant_id, child.result.object.id);
    ASSERT_EQ(source.node.kernel()->work.grants.at(1).grant_id, parent.result.object.id);
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
            const auto tail = record.body.size() - 40U;
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
            ASSERT_TRUE(ayni::authority::read_grant_snapshot_record(record.body.data(),
                                                                    record.header.length, decoded));
            ASSERT_EQ(decoded.lifecycle, AYNI_GRANT_EXPIRED);
            ASSERT_EQ(decoded.revision, 2U);
            ASSERT_GT(decoded.stamp.transition_id, decoded.created_stamp.transition_id);
            ASSERT_GT(decoded.stamp.commit_seq, decoded.created_stamp.commit_seq);
            ASSERT_LE(decoded.stamp.transition_id, snapshot.metadata.max_transition_id);
            ASSERT_LE(decoded.stamp.commit_seq, snapshot.metadata.covers_commit_seq);
            const auto tail = record.body.size() - ayni::authority::grant_snapshot_tail_size;
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

} // namespace
