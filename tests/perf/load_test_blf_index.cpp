
// =============================================================================
// FILE: tests/perf/load_test_blf_index.cpp
//
// Benchmarks the BLF subscription index under concurrent reads/writes.
//
// Run: ./load_test_blf_index [num_uris] [num_watchers_per_uri] [num_readers]
// =============================================================================
#include "subscription/blf_subscription_index.h"
#include "common/logger.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <cstdlib>

using namespace sip_processor;
using namespace std::chrono;

int main(int argc, char* argv[]) {
    int num_uris     = (argc > 1) ? atoi(argv[1]) : 10000;
    int watchers_per = (argc > 2) ? atoi(argv[2]) : 5;
    int num_readers  = (argc > 3) ? atoi(argv[3]) : 4;
    int read_ops     = 1000000;

    Logger::instance().set_level(LogLevel::kError);

    auto& idx = BlfSubscriptionIndex::instance();

    std::cout << "=== BLF Index Concurrent Load Test ===" << std::endl;
    std::cout << "URIs: " << num_uris << ", Watchers/URI: " << watchers_per
              << ", Readers: " << num_readers << std::endl;

    // Phase 1: Populate
    auto pop_start = steady_clock::now();
    for (int u = 0; u < num_uris; ++u) {
        std::string uri = "sip:" + std::to_string(u) + "@test.com";
        for (int w = 0; w < watchers_per; ++w) {
            std::string did = "dialog-" + std::to_string(u) + "-" + std::to_string(w);
            idx.add(uri, did, "test.com");
        }
    }
    auto pop_dur = duration_cast<milliseconds>(steady_clock::now() - pop_start);
    int total_entries = num_uris * watchers_per;

    std::cout << "Populated " << total_entries << " entries in " << pop_dur.count() << "ms"
              << " (" << (total_entries * 1000.0 / pop_dur.count()) << " ops/sec)" << std::endl;

    // Phase 2: Concurrent reads + writes
    std::atomic<int64_t> total_lookups{0};
    std::atomic<int64_t> total_hits{0};
    std::atomic<bool> writing{true};

    // Writer thread — continuously adds/removes entries
    std::thread writer([&]() {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(0, num_uris - 1);
        int writes = 0;

        while (writing.load()) {
            int u = dist(rng);
            std::string uri = "sip:" + std::to_string(u) + "@test.com";
            std::string did = "dialog-" + std::to_string(u) + "-churn";

            idx.add(uri, did, "test.com");
            idx.remove(uri, did);
            writes += 2;
        }
    });

    auto read_start = steady_clock::now();

    // Reader threads
    std::vector<std::thread> readers;
    int reads_per_reader = read_ops / num_readers;

    for (int r = 0; r < num_readers; ++r) {
        readers.emplace_back([&, r, reads_per_reader]() {
            std::mt19937 rng(100 + r);
            std::uniform_int_distribution<int> dist(0, num_uris * 2);  // 50% miss rate
            int hits = 0;

            for (int i = 0; i < reads_per_reader; ++i) {
                int u = dist(rng);
                std::string uri = "sip:" + std::to_string(u) + "@test.com";
                auto watchers = idx.lookup(uri);
                if (!watchers.empty()) hits++;
            }

            total_lookups.fetch_add(reads_per_reader);
            total_hits.fetch_add(hits);
        });
    }

    for (auto& t : readers) t.join();
    writing.store(false);
    writer.join();

    auto read_dur = duration_cast<milliseconds>(steady_clock::now() - read_start);
    int64_t lookups = total_lookups.load();
    int64_t hits = total_hits.load();

    std::cout << "Concurrent read/write benchmark:" << std::endl;
    std::cout << "  Lookups:    " << lookups << " in " << read_dur.count() << "ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << (lookups * 1000.0 / read_dur.count()) << " lookups/sec" << std::endl;
    std::cout << "  Per lookup: " << std::setprecision(2)
              << (read_dur.count() * 1000.0 / lookups) << " us" << std::endl;
    std::cout << "  Hit rate:   " << std::setprecision(1)
              << (hits * 100.0 / lookups) << "%" << std::endl;

    // Cleanup
    for (int u = 0; u < num_uris; ++u) {
        for (int w = 0; w < watchers_per; ++w) {
            std::string did = "dialog-" + std::to_string(u) + "-" + std::to_string(w);
            idx.remove_dialog(did);
        }
    }

    std::cout << "\nFinal index: " << idx.monitored_uri_count() << " URIs, "
              << idx.total_watcher_count() << " watchers" << std::endl;

    return 0;
}