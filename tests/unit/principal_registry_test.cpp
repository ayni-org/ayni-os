// Principals and sessions owned by authority (C-AUTH-010, C-PROD-001).

#include "authority/principal/principal_registry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ayni::authority::Principal;
using ayni::authority::PrincipalRegistry;
using ayni::authority::Session;
using ayni::authority::SessionTable;

// C-PROD-001: one owner per device.
TEST(PrincipalRegistry, holds_exactly_one_owner) {
    PrincipalRegistry principals;
    EXPECT_EQ(principals.find(7), nullptr);
    ASSERT_TRUE(principals.add_owner({7, 3, AYNI_PRINCIPAL_OWNER}));
    EXPECT_FALSE(principals.add_owner({8, 1, AYNI_PRINCIPAL_OWNER}));

    const Principal *owner = principals.find(7);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->binding_revision, 3U);
    EXPECT_EQ(principals.find(8), nullptr);
    EXPECT_EQ(principals.find(0), nullptr);
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
    EXPECT_TRUE(sessions->open({11, 7, AYNI_CHANNEL_VOICE}));
    EXPECT_TRUE(sessions->open({12, 7, AYNI_CHANNEL_MANAGEMENT}));
    EXPECT_TRUE(sessions->full());
    EXPECT_FALSE(sessions->open({13, 7, AYNI_CHANNEL_VOICE}));

    const Session *management = sessions->find(12);
    ASSERT_NE(management, nullptr);
    EXPECT_EQ(management->channel, AYNI_CHANNEL_MANAGEMENT);
    EXPECT_EQ(management->principal_id, 7U);
    EXPECT_EQ(sessions->find(13), nullptr);
}

TEST(SessionTable, closed_session_is_gone_and_frees_its_place) {
    Sessions sessions(2);
    ASSERT_TRUE(sessions->open({11, 7, AYNI_CHANNEL_VOICE}));
    ASSERT_TRUE(sessions->open({12, 7, AYNI_CHANNEL_MANAGEMENT}));
    EXPECT_TRUE(sessions->close(11));
    EXPECT_FALSE(sessions->close(11));
    EXPECT_EQ(sessions->find(11), nullptr);
    ASSERT_NE(sessions->find(12), nullptr);
    EXPECT_TRUE(sessions->open({13, 7, AYNI_CHANNEL_VOICE}));
    EXPECT_NE(sessions->find(13), nullptr);
}

} // namespace
