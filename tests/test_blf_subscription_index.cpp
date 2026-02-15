
// =============================================================================
// FILE: tests/test_blf_subscription_index.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "subscription/blf_subscription_index.h"

using namespace sip_processor;

class BlfIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean state — the singleton persists, so remove test entries in TearDown
    }
    void TearDown() override {
        auto& idx = BlfSubscriptionIndex::instance();
        idx.remove_dialog("test-dialog-1");
        idx.remove_dialog("test-dialog-2");
        idx.remove_dialog("test-dialog-3");
    }
};

TEST_F(BlfIndexTest, NormalizeStripsBrackets) {
    EXPECT_EQ(BlfSubscriptionIndex::normalize_uri("<sip:200@test.com>"), "sip:200@test.com");
}

TEST_F(BlfIndexTest, NormalizeStripsParams) {
    EXPECT_EQ(BlfSubscriptionIndex::normalize_uri("sip:200@test.com;transport=tcp"), "sip:200@test.com");
}

TEST_F(BlfIndexTest, NormalizeStripsDefaultPort) {
    EXPECT_EQ(BlfSubscriptionIndex::normalize_uri("sip:200@test.com:5060"), "sip:200@test.com");
}

TEST_F(BlfIndexTest, NormalizeLowercasesHost) {
    EXPECT_EQ(BlfSubscriptionIndex::normalize_uri("sip:User@HOST.COM"), "sip:User@host.com");
}

TEST_F(BlfIndexTest, NormalizeAddsScheme) {
    EXPECT_EQ(BlfSubscriptionIndex::normalize_uri("200@test.com"), "sip:200@test.com");
}

TEST_F(BlfIndexTest, AddAndLookup) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("sip:200@test.com", "test-dialog-1", "test.com");

    auto watchers = idx.lookup("sip:200@test.com");
    ASSERT_EQ(watchers.size(), 1u);
    EXPECT_EQ(watchers[0].dialog_id, "test-dialog-1");
    EXPECT_EQ(watchers[0].tenant_id, "test.com");
}

TEST_F(BlfIndexTest, LookupNormalizes) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("<sip:200@TEST.COM;transport=tcp>", "test-dialog-1", "test.com");

    auto watchers = idx.lookup("sip:200@test.com");
    ASSERT_EQ(watchers.size(), 1u);
}

TEST_F(BlfIndexTest, MultipleWatchersSameUri) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("sip:200@test.com", "test-dialog-1", "test.com");
    idx.add("sip:200@test.com", "test-dialog-2", "test.com");

    auto watchers = idx.lookup("sip:200@test.com");
    ASSERT_EQ(watchers.size(), 2u);
}

TEST_F(BlfIndexTest, LookupByTenant) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("sip:200@a.com", "test-dialog-1", "tenant-a");
    idx.add("sip:200@a.com", "test-dialog-2", "tenant-b");

    auto watchers = idx.lookup("sip:200@a.com", "tenant-a");
    ASSERT_EQ(watchers.size(), 1u);
    EXPECT_EQ(watchers[0].dialog_id, "test-dialog-1");
}

TEST_F(BlfIndexTest, RemoveDialog) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("sip:200@test.com", "test-dialog-1", "test.com");
    idx.add("sip:200@test.com", "test-dialog-2", "test.com");

    idx.remove_dialog("test-dialog-1");

    auto watchers = idx.lookup("sip:200@test.com");
    ASSERT_EQ(watchers.size(), 1u);
    EXPECT_EQ(watchers[0].dialog_id, "test-dialog-2");
}

TEST_F(BlfIndexTest, LookupEmptyReturnsEmpty) {
    auto& idx = BlfSubscriptionIndex::instance();
    auto watchers = idx.lookup("sip:nonexistent@test.com");
    EXPECT_TRUE(watchers.empty());
}

TEST_F(BlfIndexTest, DuplicateAddIsIdempotent) {
    auto& idx = BlfSubscriptionIndex::instance();
    idx.add("sip:200@test.com", "test-dialog-1", "test.com");
    idx.add("sip:200@test.com", "test-dialog-1", "test.com");

    auto watchers = idx.lookup("sip:200@test.com");
    ASSERT_EQ(watchers.size(), 1u);
}