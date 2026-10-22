// Principals and sessions owned by authority (C-AUTH-010, C-PROD-001).

#include "authority/principal/owner_binding_records.h"
#include "authority/principal/principal_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ayni::authority::Principal;
using ayni::authority::PrincipalRegistry;
using ayni::authority::Session;
using ayni::authority::SessionTable;

// C-PROD-001: one owner per device.
TEST(PrincipalRegistry, holds_exactly_one_owner) {
    PrincipalRegistry principals;
    EXPECT_EQ(principals.owner(), nullptr);
    EXPECT_EQ(principals.find(7), nullptr);
    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_FALSE(principals.add_owner({8, 1, AYNI_PRINCIPAL_OWNER}));

    const Principal *owner = principals.find(7);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->binding_revision, 3U);
    EXPECT_EQ(principals.owner(), owner);
    EXPECT_EQ(principals.find(8), nullptr);
    EXPECT_EQ(principals.find(0), nullptr);
}

TEST(PrincipalRegistry, invalid_owner_does_not_take_the_only_place) {
    PrincipalRegistry principals;
    EXPECT_FALSE(principals.add_owner({0, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_FALSE(principals.add_owner({7, 0, AYNI_PRINCIPAL_OWNER}));
    EXPECT_FALSE(principals.add_owner({7, 3, 0}));
    EXPECT_FALSE(principals.add_owner({7, 3, std::numeric_limits<uint32_t>::max()}));
    EXPECT_EQ(principals.owner(), nullptr);
    EXPECT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
}

TEST(PrincipalRegistry, unbound_store_needs_a_binding_only_when_the_host_has_an_owner) {
    PrincipalRegistry principals;
    EXPECT_FALSE(principals.binding_known());
    EXPECT_EQ(principals.binding(), nullptr);
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_FALSE(principals.needs_binding());
    EXPECT_FALSE(principals.bind({2, 2}));

    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_TRUE(principals.needs_binding());
    EXPECT_TRUE(principals.binding_matches());
    ASSERT_TRUE(principals.bind({2, 2}));
    EXPECT_TRUE(principals.binding_known());
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_FALSE(principals.needs_binding());
    ASSERT_NE(principals.binding(), nullptr);
    EXPECT_EQ(principals.binding()->principal_id, 7U);
    EXPECT_EQ(principals.binding()->binding_revision, 3U);
    EXPECT_EQ(principals.binding()->stamp.transition_id, 2U);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, 2U);
}

TEST(PrincipalRegistry, binding_requires_a_nonzero_stamp_and_does_not_replace_an_existing_one) {
    PrincipalRegistry principals;
    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_FALSE(principals.bind({0, 2}));
    EXPECT_FALSE(principals.bind({2, 0}));
    EXPECT_FALSE(principals.binding_known());
    ASSERT_TRUE(principals.bind({2, 2}));
    EXPECT_FALSE(principals.bind({3, 3}));
    EXPECT_FALSE(principals.restore_binding({7, 3}, {3, 3}));
    EXPECT_FALSE(principals.restore_binding({8, 4}, {3, 3}));
    EXPECT_EQ(principals.binding()->principal_id, 7U);
    EXPECT_EQ(principals.binding()->binding_revision, 3U);
    EXPECT_EQ(principals.binding()->stamp.transition_id, 2U);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, 2U);
}

TEST(PrincipalRegistry, restored_binding_is_retained_without_an_expected_owner) {
    PrincipalRegistry principals;
    ASSERT_TRUE(principals.restore_binding({7, 3}, {4, 5}));
    EXPECT_TRUE(principals.binding_known());
    EXPECT_FALSE(principals.binding_matches());
    EXPECT_FALSE(principals.needs_binding());
    EXPECT_EQ(principals.owner(), nullptr);
    EXPECT_EQ(principals.binding()->principal_id, 7U);
    EXPECT_EQ(principals.binding()->stamp.transition_id, 4U);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, 5U);
    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_TRUE(principals.binding_matches());
}

TEST(PrincipalRegistry, restored_binding_does_not_adopt_another_principal_or_revision) {
    for (const Principal expected :
         {Principal{8, 3, AYNI_PRINCIPAL_OWNER}, Principal{7, 4, AYNI_PRINCIPAL_OWNER}}) {
        PrincipalRegistry principals;
        ASSERT_TRUE(principals.add_owner(expected));
        ASSERT_TRUE(principals.restore_binding({7, 3}, {4, 5}));
        EXPECT_FALSE(principals.binding_matches());
        EXPECT_FALSE(principals.needs_binding());
        EXPECT_FALSE(principals.bind({6, 7}));
        EXPECT_EQ(principals.owner()->principal_id, expected.principal_id);
        EXPECT_EQ(principals.owner()->binding_revision, expected.binding_revision);
        EXPECT_EQ(principals.binding()->principal_id, 7U);
        EXPECT_EQ(principals.binding()->binding_revision, 3U);
    }
}

TEST(PrincipalRegistry, invalid_restored_binding_leaves_the_store_unbound) {
    PrincipalRegistry principals;
    EXPECT_FALSE(principals.restore_binding({0, 3}, {4, 5}));
    EXPECT_FALSE(principals.restore_binding({7, 0}, {4, 5}));
    EXPECT_FALSE(principals.restore_binding({7, 3}, {0, 5}));
    EXPECT_FALSE(principals.restore_binding({7, 3}, {4, 0}));
    EXPECT_FALSE(principals.binding_known());
    EXPECT_EQ(principals.binding(), nullptr);
    EXPECT_TRUE(principals.restore_binding({7, 3}, {4, 5}));
}

TEST(PrincipalRegistry, largest_owner_identity_and_revision_are_valid) {
    PrincipalRegistry principals;
    constexpr uint64_t principal_id = std::numeric_limits<uint64_t>::max();
    constexpr uint32_t revision = std::numeric_limits<uint32_t>::max();
    ASSERT_TRUE(principals.add_owner({principal_id, revision, AYNI_PRINCIPAL_OWNER}));
    ASSERT_TRUE(principals.bind({4, 5}));
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_EQ(principals.binding()->principal_id, principal_id);
    EXPECT_EQ(principals.binding()->binding_revision, revision);
}

TEST(PrincipalRegistry, reload_drops_the_binding_but_keeps_the_hosts_expected_owner) {
    PrincipalRegistry principals;
    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    ASSERT_TRUE(principals.restore_binding({8, 4}, {4, 5}));
    ASSERT_FALSE(principals.binding_matches());

    principals.drop_restored_binding();
    EXPECT_FALSE(principals.binding_known());
    EXPECT_EQ(principals.binding(), nullptr);
    ASSERT_NE(principals.owner(), nullptr);
    EXPECT_EQ(principals.owner()->principal_id, 7U);
    EXPECT_EQ(principals.owner()->binding_revision, 3U);
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_TRUE(principals.needs_binding());
    ASSERT_TRUE(principals.restore_binding({7, 3}, {6, 7}));
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_EQ(principals.binding()->stamp.transition_id, 6U);
    EXPECT_EQ(principals.binding()->stamp.commit_seq, 7U);
}

TEST(PrincipalRegistry, dropping_an_unbound_store_is_idempotent) {
    PrincipalRegistry principals;
    principals.drop_restored_binding();
    principals.drop_restored_binding();
    EXPECT_EQ(principals.owner(), nullptr);
    EXPECT_EQ(principals.binding(), nullptr);
    EXPECT_TRUE(principals.binding_matches());
    EXPECT_FALSE(principals.needs_binding());
}

class Sessions {
  public:
    explicit Sessions(uint32_t capacity)
        : storage_(SessionTable::storage_size(capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), capacity) {}
    SessionTable *operator->() { return &table_; }

  private:
    std::vector<uint64_t> storage_;
    SessionTable table_;
};

TEST(SessionTable, opens_up_to_capacity_and_finds_each) {
    Sessions sessions(2);
    EXPECT_TRUE(sessions->open({11, 7, AYNI_CHANNEL_VOICE, 3}));
    EXPECT_TRUE(sessions->open({12, 7, AYNI_CHANNEL_MANAGEMENT, 3}));
    EXPECT_TRUE(sessions->full());
    EXPECT_FALSE(sessions->open({13, 7, AYNI_CHANNEL_VOICE, 3}));

    const Session *management = sessions->find(12);
    ASSERT_NE(management, nullptr);
    EXPECT_EQ(management->channel, AYNI_CHANNEL_MANAGEMENT);
    EXPECT_EQ(management->principal_id, 7U);
    EXPECT_EQ(management->binding_revision, 3U);
    EXPECT_EQ(sessions->find(13), nullptr);
}

TEST(SessionTable, closed_session_is_gone_and_frees_its_place) {
    Sessions sessions(2);
    ASSERT_TRUE(sessions->open({11, 7, AYNI_CHANNEL_VOICE, 3}));
    ASSERT_TRUE(sessions->open({12, 7, AYNI_CHANNEL_MANAGEMENT, 3}));
    EXPECT_TRUE(sessions->close(11));
    EXPECT_FALSE(sessions->close(11));
    EXPECT_EQ(sessions->find(11), nullptr);
    ASSERT_NE(sessions->find(12), nullptr);
    EXPECT_TRUE(sessions->open({13, 7, AYNI_CHANNEL_VOICE, 3}));
    EXPECT_NE(sessions->find(13), nullptr);
}

TEST(SessionTable, clear_closes_sessions_from_the_previous_boot) {
    Sessions sessions(1);
    ASSERT_TRUE(sessions->open({11, 7, AYNI_CHANNEL_MANAGEMENT, 3}));
    sessions->clear();
    EXPECT_EQ(sessions->find(11), nullptr);
    EXPECT_FALSE(sessions->full());
    ASSERT_TRUE(sessions->open({12, 7, AYNI_CHANNEL_MANAGEMENT, 4}));
    ASSERT_NE(sessions->find(12), nullptr);
    EXPECT_EQ(sessions->find(12)->binding_revision, 4U);
}

} // namespace
