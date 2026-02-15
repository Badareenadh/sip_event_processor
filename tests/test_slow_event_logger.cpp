
// =============================================================================
// FILE: tests/test_slow_event_logger.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "common/slow_event_logger.h"
#include <thread>

using namespace sip_processor;

TEST(SlowEventLogger, NoLogBelowThreshold) {
    Config c;
    c.slow_event_warn_threshold = Millisecs(1000);  // 1s — won't trigger
    SlowEventLogger logger(c);

    {
        SlowEventLogger::Timer timer(logger, "TEST", "dialog-1");
        // Completes instantly — no log
    }

    EXPECT_EQ(logger.stats().warn_count.load(), 0u);
}

TEST(SlowEventLogger, LogsAboveWarnThreshold) {
    Config c;
    c.slow_event_warn_threshold = Millisecs(1);
    c.slow_event_error_threshold = Millisecs(10000);
    c.slow_event_critical_threshold = Millisecs(100000);
    SlowEventLogger logger(c);

    {
        SlowEventLogger::Timer timer(logger, "TEST", "dialog-1");
        std::this_thread::sleep_for(Millisecs(5));
    }

    EXPECT_GE(logger.stats().warn_count.load(), 1u);
}

TEST(SlowEventLogger, UpdateThresholdsAtRuntime) {
    Config c;
    SlowEventLogger logger(c);

    logger.set_thresholds(Millisecs(10), Millisecs(100), Millisecs(500));

    auto th = logger.thresholds();
    EXPECT_EQ(th.warn.count(), 10);
    EXPECT_EQ(th.error.count(), 100);
    EXPECT_EQ(th.critical.count(), 500);
}


// =============================================================================
// FILE: tests/test_subscription_registry.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "subscription/subscription_state.h"

using namespace sip_processor;

class RegistryTest : public ::testing::Test {
protected:
    void TearDown() override {
        auto& reg = SubscriptionRegistry::instance();
        reg.unregister_subscription("test-1");
        reg.unregister_subscription("test-2");
        reg.unregister_subscription("test-3");
    }
};

TEST_F(RegistryTest, RegisterAndLookup) {
    auto& reg = SubscriptionRegistry::instance();
    SubscriptionRegistry::SubscriptionInfo info{
        "test-1", "tenant-a", SubscriptionType::kBLF,
        SubLifecycle::kActive, Clock::now(), 0};
    reg.register_subscription("test-1", info);

    SubscriptionRegistry::SubscriptionInfo out;
    EXPECT_TRUE(reg.lookup("test-1", out));
    EXPECT_EQ(out.tenant_id, "tenant-a");
    EXPECT_EQ(out.type, SubscriptionType::kBLF);
}

TEST_F(RegistryTest, UnregisterRemoves) {
    auto& reg = SubscriptionRegistry::instance();
    SubscriptionRegistry::SubscriptionInfo info{"test-1", "t", SubscriptionType::kBLF, SubLifecycle::kActive, Clock::now(), 0};
    reg.register_subscription("test-1", info);
    reg.unregister_subscription("test-1");

    SubscriptionRegistry::SubscriptionInfo out;
    EXPECT_FALSE(reg.lookup("test-1", out));
}

TEST_F(RegistryTest, CountByTenant) {
    auto& reg = SubscriptionRegistry::instance();
    reg.register_subscription("test-1", {"test-1", "t-a", SubscriptionType::kBLF, SubLifecycle::kActive, Clock::now(), 0});
    reg.register_subscription("test-2", {"test-2", "t-a", SubscriptionType::kMWI, SubLifecycle::kActive, Clock::now(), 0});
    reg.register_subscription("test-3", {"test-3", "t-b", SubscriptionType::kBLF, SubLifecycle::kActive, Clock::now(), 0});

    EXPECT_EQ(reg.count_by_tenant("t-a"), 2u);
    EXPECT_EQ(reg.count_by_tenant("t-b"), 1u);
    EXPECT_EQ(reg.count_by_tenant("t-c"), 0u);
}

TEST_F(RegistryTest, CountByType) {
    auto& reg = SubscriptionRegistry::instance();
    reg.register_subscription("test-1", {"test-1", "t", SubscriptionType::kBLF, SubLifecycle::kActive, Clock::now(), 0});
    reg.register_subscription("test-2", {"test-2", "t", SubscriptionType::kMWI, SubLifecycle::kActive, Clock::now(), 0});

    EXPECT_GE(reg.count_by_type(SubscriptionType::kBLF), 1u);
    EXPECT_GE(reg.count_by_type(SubscriptionType::kMWI), 1u);
}
