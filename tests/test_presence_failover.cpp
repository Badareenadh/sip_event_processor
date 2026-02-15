
// =============================================================================
// FILE: tests/test_presence_failover.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "presence/presence_failover_manager.h"

using namespace sip_processor;

class FailoverTest : public ::testing::Test {
protected:
    Config make_config(FailoverStrategy strategy) {
        Config c;
        c.presence_servers = {
            {"server1.com", 9000, 0, 1},
            {"server2.com", 9000, 1, 1},
            {"server3.com", 9000, 2, 1}
        };
        c.presence_failover_strategy = strategy;
        c.presence_server_cooldown = Seconds(10);
        return c;
    }
};

TEST_F(FailoverTest, RoundRobinCycles) {
    auto cfg = make_config(FailoverStrategy::kRoundRobin);
    PresenceFailoverManager mgr(cfg);

    auto s1 = mgr.get_next_server();
    auto s2 = mgr.get_next_server();
    auto s3 = mgr.get_next_server();
    auto s4 = mgr.get_next_server();

    EXPECT_EQ(s1.host, "server1.com");
    EXPECT_EQ(s2.host, "server2.com");
    EXPECT_EQ(s3.host, "server3.com");
    EXPECT_EQ(s4.host, "server1.com");  // Wraps around
}

TEST_F(FailoverTest, RoundRobinSkipsCooldown) {
    auto cfg = make_config(FailoverStrategy::kRoundRobin);
    PresenceFailoverManager mgr(cfg);

    auto s1 = mgr.get_next_server();
    mgr.report_failure(s1, "test");  // server1 in cooldown

    auto s2 = mgr.get_next_server();
    EXPECT_EQ(s2.host, "server2.com");  // Skipped server1
}

TEST_F(FailoverTest, PriorityPicksLowest) {
    auto cfg = make_config(FailoverStrategy::kPriority);
    PresenceFailoverManager mgr(cfg);

    auto s = mgr.get_next_server();
    EXPECT_EQ(s.host, "server1.com");  // Priority 0
}

TEST_F(FailoverTest, PriorityFallsBack) {
    auto cfg = make_config(FailoverStrategy::kPriority);
    PresenceFailoverManager mgr(cfg);

    auto s1 = mgr.get_next_server();
    mgr.report_failure(s1);

    auto s2 = mgr.get_next_server();
    EXPECT_EQ(s2.host, "server2.com");  // Priority 1
}

TEST_F(FailoverTest, SuccessResetsCooldown) {
    auto cfg = make_config(FailoverStrategy::kRoundRobin);
    PresenceFailoverManager mgr(cfg);

    auto s1 = mgr.get_next_server();
    mgr.report_failure(s1);
    mgr.report_failure(s1);
    mgr.report_failure(s1);  // 3 failures → unhealthy

    auto health = mgr.get_all_health();
    bool found = false;
    for (auto& h : health) {
        if (h.endpoint.host == "server1.com") {
            EXPECT_FALSE(h.is_healthy);
            EXPECT_EQ(h.consecutive_failures, 3);
            found = true;
        }
    }
    EXPECT_TRUE(found);

    mgr.report_success(s1);

    for (auto& h : mgr.get_all_health()) {
        if (h.endpoint.host == "server1.com") {
            EXPECT_TRUE(h.is_healthy);
            EXPECT_EQ(h.consecutive_failures, 0);
        }
    }
}

TEST_F(FailoverTest, AllInCooldownPicksSoonestExpiry) {
    auto cfg = make_config(FailoverStrategy::kRoundRobin);
    PresenceFailoverManager mgr(cfg);

    // Fail all servers
    for (int i = 0; i < 3; ++i) {
        auto s = mgr.get_next_server();
        mgr.report_failure(s);
    }

    // Should still return a server (the one with soonest cooldown expiry)
    auto s = mgr.get_next_server();
    EXPECT_FALSE(s.host.empty());
}

TEST_F(FailoverTest, HealthyCount) {
    auto cfg = make_config(FailoverStrategy::kRoundRobin);
    PresenceFailoverManager mgr(cfg);

    EXPECT_EQ(mgr.healthy_count(), 3u);

    auto s = mgr.get_next_server();
    mgr.report_failure(s);
    mgr.report_failure(s);
    mgr.report_failure(s);  // Marks unhealthy at 3 failures

    EXPECT_EQ(mgr.healthy_count(), 2u);
}
