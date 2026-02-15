

// =============================================================================
// FILE: include/presence/presence_xml_parser.h
// =============================================================================
#ifndef PRESENCE_XML_PARSER_H
#define PRESENCE_XML_PARSER_H

#include "presence/call_state_event.h"
#include <string>
#include <vector>

namespace sip_processor {

class PresenceXmlParser {
public:
    PresenceXmlParser();
    ~PresenceXmlParser();

    struct ParseResult {
        std::vector<CallStateEvent> events;
        bool received_heartbeat = false;
        size_t bytes_consumed   = 0;
        std::string error;
    };

    ParseResult feed(const char* data, size_t len);
    void reset();

    uint64_t total_events_parsed() const { return total_parsed_; }
    uint64_t total_parse_errors()  const { return total_errors_; }

    PresenceXmlParser(const PresenceXmlParser&) = delete;
    PresenceXmlParser& operator=(const PresenceXmlParser&) = delete;

private:
    CallStateEvent parse_single_event(const std::string& xml);
    CallState parse_call_state(const std::string& state_str);
    std::string extract_element(const std::string& xml, const std::string& tag) const;

    std::string buffer_;
    size_t max_buffer_size_ = 1048576;
    uint64_t total_parsed_ = 0;
    uint64_t total_errors_ = 0;
};

} // namespace sip_processor
#endif
