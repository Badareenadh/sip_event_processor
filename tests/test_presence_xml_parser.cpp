
// =============================================================================
// FILE: tests/test_presence_xml_parser.cpp
// =============================================================================
#include <gtest/gtest.h>
#include "presence/presence_xml_parser.h"

using namespace sip_processor;

TEST(PresenceXmlParser, ParseSingleEvent) {
    PresenceXmlParser parser;
    const char* xml =
        "<CallStateEvent>"
        "<CallId>call-123</CallId>"
        "<CallerUri>sip:100@test.com</CallerUri>"
        "<CalleeUri>sip:200@test.com</CalleeUri>"
        "<State>confirmed</State>"
        "<Direction>inbound</Direction>"
        "<TenantId>test.com</TenantId>"
        "<Timestamp>2026-02-14T10:00:00Z</Timestamp>"
        "</CallStateEvent>";

    auto result = parser.feed(xml, strlen(xml));
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_TRUE(result.events[0].is_valid);
    EXPECT_EQ(result.events[0].presence_call_id, "call-123");
    EXPECT_EQ(result.events[0].caller_uri, "sip:100@test.com");
    EXPECT_EQ(result.events[0].callee_uri, "sip:200@test.com");
    EXPECT_EQ(result.events[0].state, CallState::kConfirmed);
    EXPECT_EQ(result.events[0].tenant_id, "test.com");
}

TEST(PresenceXmlParser, ParseMultipleEvents) {
    PresenceXmlParser parser;
    std::string xml =
        "<CallStateEvent><CallId>c1</CallId><CallerUri>a</CallerUri>"
        "<CalleeUri>b</CalleeUri><State>trying</State></CallStateEvent>"
        "<CallStateEvent><CallId>c2</CallId><CallerUri>c</CallerUri>"
        "<CalleeUri>d</CalleeUri><State>ringing</State></CallStateEvent>";

    auto result = parser.feed(xml.c_str(), xml.size());
    ASSERT_EQ(result.events.size(), 2u);
    EXPECT_EQ(result.events[0].state, CallState::kTrying);
    EXPECT_EQ(result.events[1].state, CallState::kRinging);
}

TEST(PresenceXmlParser, ParseIncompleteBuffers) {
    PresenceXmlParser parser;
    const char* part1 = "<CallStateEvent><CallId>c1</Call";
    const char* part2 = "Id><CallerUri>a</CallerUri><CalleeUri>b</CalleeUri>"
                        "<State>confirmed</State></CallStateEvent>";

    auto r1 = parser.feed(part1, strlen(part1));
    EXPECT_EQ(r1.events.size(), 0u);

    auto r2 = parser.feed(part2, strlen(part2));
    ASSERT_EQ(r2.events.size(), 1u);
    EXPECT_EQ(r2.events[0].presence_call_id, "c1");
}

TEST(PresenceXmlParser, ParseHeartbeat) {
    PresenceXmlParser parser;
    const char* xml = "<Heartbeat><Timestamp>2026-02-14T10:00:00Z</Timestamp></Heartbeat>";
    auto result = parser.feed(xml, strlen(xml));
    EXPECT_TRUE(result.received_heartbeat);
    EXPECT_EQ(result.events.size(), 0u);
}

TEST(PresenceXmlParser, RejectInvalidEvent) {
    PresenceXmlParser parser;
    // Missing CallId
    const char* xml = "<CallStateEvent><CallerUri>a</CallerUri>"
                      "<CalleeUri>b</CalleeUri><State>trying</State></CallStateEvent>";
    auto result = parser.feed(xml, strlen(xml));
    EXPECT_EQ(result.events.size(), 0u);
}

TEST(PresenceXmlParser, ParseCallStates) {
    PresenceXmlParser parser;
    auto make_event = [](const char* state) -> std::string {
        return std::string("<CallStateEvent><CallId>c</CallId><CallerUri>a</CallerUri>"
               "<CalleeUri>b</CalleeUri><State>") + state + "</State></CallStateEvent>";
    };

    auto test = [&parser](const std::string& xml, CallState expected) {
        parser.reset();
        auto r = parser.feed(xml.c_str(), xml.size());
        ASSERT_EQ(r.events.size(), 1u);
        EXPECT_EQ(r.events[0].state, expected);
    };

    test(make_event("trying"), CallState::kTrying);
    test(make_event("ringing"), CallState::kRinging);
    test(make_event("alerting"), CallState::kRinging);
    test(make_event("confirmed"), CallState::kConfirmed);
    test(make_event("connected"), CallState::kConfirmed);
    test(make_event("active"), CallState::kConfirmed);
    test(make_event("terminated"), CallState::kTerminated);
    test(make_event("disconnected"), CallState::kTerminated);
    test(make_event("held"), CallState::kHeld);
    test(make_event("resumed"), CallState::kResumed);
}

TEST(PresenceXmlParser, ResetClearsBuffer) {
    PresenceXmlParser parser;
    parser.feed("<CallStateEvent><Call", 20);
    parser.reset();
    // After reset, incomplete data should not interfere
    const char* xml = "<CallStateEvent><CallId>fresh</CallId><CallerUri>a</CallerUri>"
                      "<CalleeUri>b</CalleeUri><State>trying</State></CallStateEvent>";
    auto r = parser.feed(xml, strlen(xml));
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_EQ(r.events[0].presence_call_id, "fresh");
}
