// ADR-0062: actual Mock bytes have independent object/byte bounds, exact
// owner/purpose identity and explicit expiry, generation and boot reclamation.
#include <ayni/providers/mock/media.h>
#include <cstring>
#include <gtest/gtest.h>
namespace {
using ayni::providers::mock::MediaStore;
ayni_media_ref_v1 metadata(uint64_t generation = 1) {
    ayni_media_ref_v1 value{};
    value.struct_size = sizeof(value);
    value.content_type = AYNI_MEDIA_MOCK_BYTES;
    value.boot_epoch = 1;
    value.owner_instance_id = 10;
    value.generation = generation;
    value.byte_size = 3;
    value.valid_until_us = 100;
    value.source_id.size = 6;
    std::memcpy(value.source_id.data, "camera", 6);
    value.source_version = 1;
    value.purpose.size = 7;
    std::memcpy(value.purpose.data, "inspect", 7);
    return value;
}
TEST(MockMedia, copied_bytes_require_exact_reference_owner_and_purpose) {
    MediaStore store;
    ASSERT_EQ(store.configure({1, 3}), AYNI_OK);
    ASSERT_EQ(store.begin_boot(1), AYNI_OK);
    uint8_t bytes[]{1, 2, 3};
    ayni_media_ref_v1 reference{};
    ASSERT_EQ(store.put(metadata(), {bytes, sizeof(bytes)}, 0, reference), AYNI_OK);
    bytes[0] = 9;
    uint8_t out[3]{};
    uint32_t size = 99;
    EXPECT_EQ(store.read(reference, 11, {"inspect", 7}, 0, out, sizeof(out), size), AYNI_DENIED);
    EXPECT_EQ(size, 99U);
    EXPECT_EQ(store.read(reference, 10, {"upload", 6}, 0, out, sizeof(out), size), AYNI_DENIED);
    EXPECT_EQ(store.read(reference, 10, {"inspect", 7}, 0, out, 2, size), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, 3U);
    EXPECT_EQ(out[0], 0U);
    EXPECT_EQ(store.read(reference, 10, {"inspect", 7}, 0, out, sizeof(out), size), AYNI_OK);
    EXPECT_EQ(out[0], 1U);
    ++reference.valid_until_us;
    EXPECT_EQ(store.read(reference, 10, {"inspect", 7}, 0, out, sizeof(out), size), AYNI_NOT_FOUND);
}
TEST(MockMedia, object_and_byte_limits_reject_without_overwriting_and_expiry_reclaims) {
    MediaStore store;
    ASSERT_EQ(store.configure({1, 3}), AYNI_OK);
    ASSERT_EQ(store.begin_boot(1), AYNI_OK);
    const uint8_t bytes[]{1, 2, 3, 4};
    auto value = metadata();
    ayni_media_ref_v1 first{};
    value.byte_size = 4;
    EXPECT_EQ(store.put(value, {bytes, 4}, 0, first), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(store.occupied(), 0U);
    ASSERT_EQ(store.put(metadata(), {bytes, 3}, 0, first), AYNI_OK);
    ayni_media_ref_v1 second{};
    EXPECT_EQ(store.put(metadata(), {bytes, 3}, 0, second), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(second.media_id, 0U);
    auto later = metadata();
    later.valid_until_us = 200;
    ASSERT_EQ(store.put(later, {bytes, 3}, 100, second), AYNI_OK);
    EXPECT_GT(second.media_id, first.media_id);
    uint8_t out[3]{};
    uint32_t size = 0;
    EXPECT_EQ(store.read(first, 10, {"inspect", 7}, 100, out, 3, size), AYNI_EXPIRED);
    EXPECT_EQ(store.occupied(), 1U);
    store.expire(200);
    EXPECT_EQ(store.occupied(), 0U);
}
TEST(MockMedia, generation_close_and_new_boot_reclaim_without_reusing_identity) {
    MediaStore store;
    ASSERT_EQ(store.configure({2, 3}), AYNI_OK);
    ASSERT_EQ(store.begin_boot(1), AYNI_OK);
    const uint8_t bytes[]{1, 2, 3};
    ayni_media_ref_v1 first{}, second{};
    ASSERT_EQ(store.put(metadata(), {bytes, 3}, 0, first), AYNI_OK);
    ASSERT_EQ(store.put(metadata(2), {bytes, 3}, 0, second), AYNI_OK);
    store.release_generation({"camera", 6}, 1, 1);
    EXPECT_EQ(store.occupied(), 1U);
    uint8_t out[3]{};
    uint32_t size = 0;
    EXPECT_EQ(store.read(first, 10, {"inspect", 7}, 0, out, 3, size), AYNI_NOT_FOUND);
    ASSERT_EQ(store.begin_boot(2), AYNI_OK);
    EXPECT_EQ(store.occupied(), 0U);
    EXPECT_EQ(store.begin_boot(1), AYNI_STALE);
    EXPECT_EQ(store.read(second, 10, {"inspect", 7}, 0, out, 3, size), AYNI_STALE);
    auto current = metadata();
    current.boot_epoch = 2;
    ayni_media_ref_v1 third{};
    ASSERT_EQ(store.put(current, {bytes, 3}, 0, third), AYNI_OK);
    ASSERT_EQ(store.begin_boot(2), AYNI_OK);
    EXPECT_EQ(store.occupied(), 1U);
    EXPECT_GT(third.media_id, second.media_id);
}
TEST(MockMedia, startup_and_schema_failures_are_explicit) {
    MediaStore store;
    EXPECT_EQ(store.begin_boot(1), AYNI_NOT_READY);
    EXPECT_EQ(store.configure({0, 3}), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(store.configure({1, 3}), AYNI_OK);
    EXPECT_EQ(store.configure({1, 3}), AYNI_INVALID_STATE);
    EXPECT_EQ(store.begin_boot(0), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(store.begin_boot(1), AYNI_OK);
    auto value = metadata();
    value.media_id = 1;
    EXPECT_EQ(ayni_validate_media_reference(&value), AYNI_OK);
    ++value.struct_size;
    EXPECT_EQ(ayni_validate_media_reference(&value), AYNI_UNSUPPORTED_VERSION);
    value = metadata();
    value.media_id = 1;
    value.purpose.size = AYNI_CATALOG_ID_MAX_BYTES + 1;
    EXPECT_EQ(ayni_validate_media_reference(&value), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_validate_media_reference(nullptr), AYNI_INVALID_ARGUMENT);
}
} // namespace
