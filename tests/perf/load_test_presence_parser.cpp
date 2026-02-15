
// =============================================================================
// FILE: tests/perf/load_test_presence_parser.cpp
//
// Benchmarks the XML parser throughput for presence events.
//
// Run: ./load_test_presence_parser [num_events]
// =============================================================================
#include "presence/presence_xml_parser.h"
#include "common/logger.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>

using namespace sip_processor;
using namespace std::chrono;

int main(int argc, char* argv[]) {
    int num_events = (argc > 1) ? atoi(argv[1]) : 500000;

    Logger::instance().set_level(LogLevel::kError);

    std::cout << "=== Presence XML Parser Load Test ===" << std::endl;
    std::cout << "Events: " << num_events << std::endl;

    // Pre-generate XML payload (batch of 10 events per chunk)
    std::string chunk;
    for (int i = 0; i < 10; ++i) {
        chunk += "<CallStateEvent>"
                 "<CallId>call-" + std::to_string(i) + "</CallId>"
                 "<CallerUri>sip:" + std::to_string(100 + i) + "@test.com</CallerUri>"
                 "<CalleeUri>sip:" + std::to_string(200 + i) + "@test.com</CalleeUri>"
                 "<State>confirmed</State>"
                 "<Direction>inbound</Direction>"
                 "<TenantId>test.com</TenantId>"
                 "<Timestamp>2026-02-14T10:00:00Z</Timestamp>"
                 "</CallStateEvent>\n";
    }

    int chunks_needed = num_events / 10;
    PresenceXmlParser parser;
    int total_parsed = 0;

    auto start = steady_clock::now();

    for (int i = 0; i < chunks_needed; ++i) {
        auto result = parser.feed(chunk.c_str(), chunk.size());
        total_parsed += result.events.size();
    }

    auto dur = duration_cast<milliseconds>(steady_clock::now() - start);

    std::cout << "Parsed:     " << total_parsed << " events" << std::endl;
    std::cout << "Duration:   " << dur.count() << " ms" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(0)
              << (total_parsed * 1000.0 / dur.count()) << " events/sec" << std::endl;
    std::cout << "Per event:  " << std::setprecision(2)
              << (dur.count() * 1000.0 / total_parsed) << " us/event" << std::endl;
    std::cout << "Chunk size: " << chunk.size() << " bytes ("
              << std::setprecision(1)
              << ((chunks_needed * chunk.size()) / 1048576.0) << " MB total)" << std::endl;

    return 0;
}
