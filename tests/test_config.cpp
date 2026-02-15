
// =============================================================================
// FILE: tests/test_config.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "common/config.h"
#include <fstream>

using namespace sip_processor;

TEST(Config, LoadDefaults) {
    auto c = Config::load_defaults();
    EXPECT_GT(c.num_workers, 0u);
    EXPECT_EQ(c.sip_bind_url, "sip:*:5060");
    EXPECT_FALSE(c.presence_servers.empty());
}

TEST(Config, LoadFromFile) {
    // Write temp config
    const char* path = "/tmp/test_sip_proc.conf";
    std::ofstream f(path);
    f << "[general]\nservice_id = test-svc\nlog_level = debug\n\n"
      << "[dispatcher]\nnum_workers = 4\n\n"
      << "[presence]\nservers = host1:9001,host2:9002\n"
      << "failover_strategy = priority\n\n"
      << "[mongodb]\nenable_persistence = false\n";
    f.close();

    auto c = Config::load_from_file(path);
    EXPECT_EQ(c.service_id, "test-svc");
    EXPECT_EQ(c.log_level_str, "debug");
    EXPECT_EQ(c.num_workers, 4u);
    EXPECT_EQ(c.presence_servers.size(), 2u);
    EXPECT_EQ(c.presence_servers[0].host, "host1");
    EXPECT_EQ(c.presence_servers[0].port, 9001);
    EXPECT_EQ(c.presence_servers[1].host, "host2");
    EXPECT_EQ(c.presence_failover_strategy, FailoverStrategy::kPriority);
    EXPECT_FALSE(c.mongo_enable_persistence);

    remove(path);
}

TEST(Config, ParseServersCsv) {
    // Access via load_from_file with servers line
    const char* path = "/tmp/test_servers.conf";
    std::ofstream f(path);
    f << "[presence]\nservers = a.com:9000, b.com:9001 , c.com , :invalid\n";
    f.close();

    auto c = Config::load_from_file(path);
    EXPECT_GE(c.presence_servers.size(), 3u);
    EXPECT_EQ(c.presence_servers[0].host, "a.com");
    EXPECT_EQ(c.presence_servers[0].port, 9000);
    EXPECT_EQ(c.presence_servers[1].port, 9001);

    remove(path);
}
