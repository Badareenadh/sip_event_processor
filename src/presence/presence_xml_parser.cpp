
// =============================================================================
// FILE: src/presence/presence_xml_parser.cpp
// =============================================================================
#include "presence/presence_xml_parser.h"
#include "common/logger.h"
#include <algorithm>
#include <cctype>

namespace sip_processor {

std::atomic<EventId> CallStateEvent::id_counter_{0};
EventId CallStateEvent::next_id() { return id_counter_.fetch_add(1, std::memory_order_relaxed) + 1; }

PresenceXmlParser::PresenceXmlParser() { buffer_.reserve(4096); }
PresenceXmlParser::~PresenceXmlParser() = default;
void PresenceXmlParser::reset() { buffer_.clear(); }

std::string PresenceXmlParser::extract_element(const std::string& xml, const std::string& tag) const {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    auto s = xml.find(open);
    if (s == std::string::npos) return "";
    s += open.size();
    auto e = xml.find(close, s);
    if (e == std::string::npos) return "";
    std::string val = xml.substr(s, e - s);
    val.erase(0, val.find_first_not_of(" \t\n\r"));
    val.erase(val.find_last_not_of(" \t\n\r") + 1);
    return val;
}

CallState PresenceXmlParser::parse_call_state(const std::string& s) {
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    if (l == "trying" || l == "setup")                        return CallState::kTrying;
    if (l == "ringing" || l == "early" || l == "alerting")    return CallState::kRinging;
    if (l == "confirmed" || l == "connected" || l == "active") return CallState::kConfirmed;
    if (l == "terminated" || l == "disconnected" || l == "released" || l == "idle") return CallState::kTerminated;
    if (l == "held" || l == "hold") return CallState::kHeld;
    if (l == "resumed")             return CallState::kResumed;
    return CallState::kUnknown;
}

CallStateEvent PresenceXmlParser::parse_single_event(const std::string& xml) {
    CallStateEvent ev;
    ev.id = CallStateEvent::next_id();
    ev.received_at = Clock::now();
    ev.presence_call_id = extract_element(xml, "CallId");
    ev.caller_uri       = extract_element(xml, "CallerUri");
    ev.callee_uri       = extract_element(xml, "CalleeUri");
    ev.direction        = extract_element(xml, "Direction");
    ev.tenant_id        = extract_element(xml, "TenantId");
    ev.timestamp_str    = extract_element(xml, "Timestamp");
    ev.state = parse_call_state(extract_element(xml, "State"));

    ev.is_valid = !ev.presence_call_id.empty() &&
                  (!ev.callee_uri.empty() || !ev.caller_uri.empty()) &&
                  ev.state != CallState::kUnknown;

    if (!ev.is_valid) LOG_WARN("PresenceParser: invalid event (call=%s)", ev.presence_call_id.c_str());
    return ev;
}

PresenceXmlParser::ParseResult PresenceXmlParser::feed(const char* data, size_t len) {
    ParseResult result;
    if (!data || len == 0) return result;

    if (buffer_.size() + len > max_buffer_size_) {
        LOG_ERROR("PresenceParser: buffer overflow, resetting");
        buffer_.clear();
        result.error = "Buffer overflow";
        total_errors_++;
        return result;
    }

    buffer_.append(data, len);
    result.bytes_consumed = len;

    const std::string open_tag = "<CallStateEvent>", close_tag = "</CallStateEvent>";
    size_t search_pos = 0;

    while (true) {
        auto s = buffer_.find(open_tag, search_pos);
        if (s == std::string::npos) break;
        auto e = buffer_.find(close_tag, s);
        if (e == std::string::npos) break;
        e += close_tag.size();

        auto ev = parse_single_event(buffer_.substr(s, e - s));
        if (ev.is_valid) { result.events.push_back(std::move(ev)); total_parsed_++; }
        else total_errors_++;
        search_pos = e;
    }

    // Heartbeat
    auto hb_s = buffer_.find("<Heartbeat>", search_pos);
    if (hb_s != std::string::npos) {
        auto hb_e = buffer_.find("</Heartbeat>", hb_s);
        if (hb_e != std::string::npos) {
            result.received_heartbeat = true;
            search_pos = hb_e + 12;
        }
    }

    if (search_pos > 0) buffer_.erase(0, search_pos);
    if (!buffer_.empty()) {
        auto lt = buffer_.find('<');
        if (lt == std::string::npos) buffer_.clear();
        else if (lt > 0) buffer_.erase(0, lt);
    }
    return result;
}

} // namespace sip_processor

