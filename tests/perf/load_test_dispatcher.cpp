// =============================================================================
// FILE: tests/perf/load_test_dispatcher.cpp
//
// Load test for the Dialog Dispatcher — measures throughput, latency,
// and memory under high event rates without SIP stack dependency.
//
// Build:
//   g++ -O2 -std=c++17 -pthread load_test_dispatcher.cpp \
//       ../../src/dispatch/*.cpp ../../src/subscription/*.cpp \
//       ../../src/common/*.cpp ../../src/sip/sip_event.cpp \
//       ../../src/sip/sip_dialog_id.cpp \
//       -I../../include -lmongocxx -lbsoncxx -o load_test_dispatcher
//
// Run:
//   ./load_test_dispatcher [num_events] [num_dialogs] [num_workers]
// =============================================================================
#include "common/config.h"
#include "common/logger.h"
#include "common/slow_event_logger.h"
#include "dispatch/dialog_dispatcher.h"
#include "persistence/subscription_store.h"
#include "sip/sip_event.h"
#include "subscription/subscription_state.h"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <cstring>

using namespace sip_processor;
using namespace std::chrono;

// Generate a unique dialog ID
static std::string make_dialog_id(int tenant, int sub) {
    char buf[128];
    snprintf(buf, sizeof(buf), "callid-%d-%d;ft=from%d;tt=to%d",
             tenant, sub, tenant, sub);
    return std::string(buf);
}

// Generate a synthetic SIP event
static std::unique_ptr<SipEvent> make_event(const std::string& dialog_id,
                                              const std::string& tenant_id,
                                              SipEventCategory cat) {
    auto ev = std::make_unique<SipEvent>();
    ev->id = SipEvent::next_id();
    ev->dialog_id = dialog_id;
    ev->tenant_id = tenant_id;
    ev->category = cat;
    ev->source = SipEventSource::kSipStack;
    ev->sub_type = SubscriptionType::kBLF;
    ev->direction = SipDirection::kIncoming;
    ev->created_at = Clock::now();
    ev->expires = 3600;
    ev->subscription_state = "active";
    ev->to_uri = "sip:monitored@" + tenant_id;
    ev->from_uri = "sip:watcher@" + tenant_id;
    return ev;
}

static std::unique_ptr<SipEvent> make_presence_trigger(const std::string& dialog_id,
                                                         const std::string& tenant_id) {
    return SipEvent::create_presence_trigger(
        dialog_id, tenant_id, "presence-call-" + dialog_id,
        "sip:caller@" + tenant_id, "sip:callee@" + tenant_id,
        "confirmed", "inbound", "<dialog-info/>");
}

int main(int argc, char* argv[]) {
    // Defaults
    int total_events  = (argc > 1) ? atoi(argv[1]) : 1000000;
    int num_dialogs   = (argc > 2) ? atoi(argv[2]) : 100000;
    int num_workers   = (argc > 3) ? atoi(argv[3]) : 0;
    int num_producers = 4;

    Logger::instance().set_level(LogLevel::kWarn);

    Config config = Config::load_defaults();
    if (num_workers > 0) config.num_workers = static_cast<size_t>(num_workers);
    config.max_incoming_queue_per_worker = 500000;
    config.max_subscriptions_per_tenant = 1000000;
    config.max_dialogs_per_worker = 5000000;
    config.mongo_enable_persistence = false;

    auto slow_logger = std::make_shared<SlowEventLogger>(config);
    auto sub_store = std::make_shared<SubscriptionStore>(config, nullptr);

    DialogDispatcher dispatcher(config, slow_logger, sub_store);
    dispatcher.start();

    std::cout << "=== SIP Event Processor Load Test ===" << std::endl;
    std::cout << "Events:    " << total_events << std::endl;
    std::cout << "Dialogs:   " << num_dialogs << std::endl;
    std::cout << "Workers:   " << config.num_workers << std::endl;
    std::cout << "Producers: " << num_producers << std::endl;
    std::cout << std::endl;

    // Pre-generate dialog IDs
    std::vector<std::string> dialog_ids(num_dialogs);
    std::vector<std::string> tenant_ids(num_dialogs);
    for (int i = 0; i < num_dialogs; ++i) {
        int tenant = i / 1000;
        int sub = i % 1000;
        dialog_ids[i] = make_dialog_id(tenant, sub);
        tenant_ids[i] = "tenant-" + std::to_string(tenant) + ".com";
    }

    // ─── Phase 1: Subscription creation ───
    std::cout << "Phase 1: Creating " << num_dialogs << " subscriptions..." << std::endl;
    auto phase1_start = steady_clock::now();

    for (int i = 0; i < num_dialogs; ++i) {
        auto ev = make_event(dialog_ids[i], tenant_ids[i], SipEventCategory::kSubscribe);
        dispatcher.dispatch(std::move(ev));
    }

    // Wait for processing
    std::this_thread::sleep_for(milliseconds(2000));

    auto phase1_dur = duration_cast<milliseconds>(steady_clock::now() - phase1_start);
    auto agg1 = dispatcher.aggregate_stats();
    std::cout << "  Created " << agg1.total_dialogs_active << " dialogs in "
              << phase1_dur.count() << "ms" << std::endl;
    std::cout << "  Rate: " << (num_dialogs * 1000.0 / phase1_dur.count())
              << " subs/sec" << std::endl;
    std::cout << std::endl;

    // ─── Phase 2: High-throughput event processing ───
    std::cout << "Phase 2: Processing " << total_events << " events across "
              << num_producers << " producer threads..." << std::endl;

    std::atomic<int64_t> events_sent{0};
    std::atomic<int64_t> events_failed{0};
    std::atomic<int64_t> total_enqueue_ns{0};
    std::atomic<int64_t> max_enqueue_ns{0};

    auto phase2_start = steady_clock::now();

    // Producer threads
    std::vector<std::thread> producers;
    int events_per_producer = total_events / num_producers;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p, events_per_producer]() {
            std::mt19937 rng(42 + p);
            std::uniform_int_distribution<int> dialog_dist(0, num_dialogs - 1);
            std::uniform_int_distribution<int> type_dist(0, 2);

            for (int i = 0; i < events_per_producer; ++i) {
                int idx = dialog_dist(rng);
                std::unique_ptr<SipEvent> ev;

                int type = type_dist(rng);
                if (type == 0) {
                    ev = make_event(dialog_ids[idx], tenant_ids[idx],
                                    SipEventCategory::kNotify);
                } else if (type == 1) {
                    ev = make_presence_trigger(dialog_ids[idx], tenant_ids[idx]);
                } else {
                    ev = make_event(dialog_ids[idx], tenant_ids[idx],
                                    SipEventCategory::kSubscribe);
                    ev->expires = 3600;
                }

                auto enq_start = steady_clock::now();
                Result r = dispatcher.dispatch(std::move(ev));
                auto enq_ns = duration_cast<nanoseconds>(steady_clock::now() - enq_start).count();

                total_enqueue_ns.fetch_add(enq_ns, std::memory_order_relaxed);
                int64_t prev_max = max_enqueue_ns.load(std::memory_order_relaxed);
                while (enq_ns > prev_max) {
                    if (max_enqueue_ns.compare_exchange_weak(prev_max, enq_ns)) break;
                }

                if (r == Result::kOk) events_sent.fetch_add(1, std::memory_order_relaxed);
                else events_failed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : producers) t.join();

    // Wait for workers to drain
    std::cout << "  Waiting for workers to drain..." << std::endl;
    std::this_thread::sleep_for(milliseconds(5000));

    auto phase2_dur = duration_cast<milliseconds>(steady_clock::now() - phase2_start);
    auto agg2 = dispatcher.aggregate_stats();

    int64_t sent = events_sent.load();
    int64_t failed = events_failed.load();
    double avg_enqueue_us = (total_enqueue_ns.load() / 1000.0) / std::max(sent, 1LL);

    std::cout << std::endl;
    std::cout << "=== Phase 2 Results ===" << std::endl;
    std::cout << "  Duration:          " << phase2_dur.count() << " ms" << std::endl;
    std::cout << "  Events sent:       " << sent << std::endl;
    std::cout << "  Events failed:     " << failed << std::endl;
    std::cout << "  Events processed:  " << agg2.total_events_processed << std::endl;
    std::cout << "  Events dropped:    " << agg2.total_events_dropped << std::endl;
    std::cout << "  Throughput:        " << std::fixed << std::setprecision(0)
              << (sent * 1000.0 / phase2_dur.count()) << " events/sec" << std::endl;
    std::cout << "  Avg enqueue lat:   " << std::setprecision(2)
              << avg_enqueue_us << " us" << std::endl;
    std::cout << "  Max enqueue lat:   "
              << (max_enqueue_ns.load() / 1000.0) << " us" << std::endl;
    std::cout << "  Active dialogs:    " << agg2.total_dialogs_active << std::endl;
    std::cout << "  Max queue depth:   " << agg2.max_queue_depth << std::endl;
    std::cout << "  Slow events:       " << agg2.total_slow_events << std::endl;
    std::cout << "  Presence triggers: " << agg2.total_presence_triggers << std::endl;
    std::cout << std::endl;

    // ─── Phase 3: Per-worker breakdown ───
    std::cout << "=== Per-Worker Stats ===" << std::endl;
    std::cout << std::setw(8) << "Worker" << std::setw(12) << "Received"
              << std::setw(12) << "Processed" << std::setw(10) << "Dropped"
              << std::setw(10) << "Dialogs" << std::setw(10) << "QDepth"
              << std::setw(10) << "Slow" << std::endl;

    for (size_t i = 0; i < dispatcher.num_workers(); ++i) {
        auto& s = dispatcher.worker(i).stats();
        std::cout << std::setw(8) << i
                  << std::setw(12) << s.events_received.load()
                  << std::setw(12) << s.events_processed.load()
                  << std::setw(10) << s.events_dropped.load()
                  << std::setw(10) << s.dialogs_active.load()
                  << std::setw(10) << s.queue_depth.load()
                  << std::setw(10) << s.slow_events.load()
                  << std::endl;
    }

    // ─── Phase 4: BLF Index performance ───
    std::cout << std::endl;
    std::cout << "=== BLF Index Stats ===" << std::endl;
    auto& idx = BlfSubscriptionIndex::instance();
    std::cout << "  Monitored URIs:  " << idx.monitored_uri_count() << std::endl;
    std::cout << "  Total watchers:  " << idx.total_watcher_count() << std::endl;

    // Benchmark index lookups
    {
        int lookup_count = 100000;
        auto lk_start = steady_clock::now();
        std::mt19937 rng(999);
        std::uniform_int_distribution<int> dist(0, num_dialogs - 1);
        int found = 0;

        for (int i = 0; i < lookup_count; ++i) {
            int idx_val = dist(rng);
            auto watchers = idx.lookup("sip:monitored@tenant-" +
                                        std::to_string(idx_val / 1000) + ".com");
            if (!watchers.empty()) found++;
        }

        auto lk_dur = duration_cast<microseconds>(steady_clock::now() - lk_start);
        std::cout << "  Lookup benchmark: " << lookup_count << " lookups in "
                  << lk_dur.count() << " us ("
                  << (lk_dur.count() * 1.0 / lookup_count) << " us/lookup, "
                  << found << " hits)" << std::endl;
    }

    // Cleanup
    dispatcher.stop();

    std::cout << std::endl;
    std::cout << "=== Slow Event Logger Stats ===" << std::endl;
    std::cout << "  Warn:     " << slow_logger->stats().warn_count.load() << std::endl;
    std::cout << "  Error:    " << slow_logger->stats().error_count.load() << std::endl;
    std::cout << "  Critical: " << slow_logger->stats().critical_count.load() << std::endl;
    std::cout << "  Max ms:   " << slow_logger->stats().max_duration_ms.load() << std::endl;

    std::cout << std::endl << "Load test complete." << std::endl;
    return 0;
}
