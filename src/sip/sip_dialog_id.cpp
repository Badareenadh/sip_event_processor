
// =============================================================================
// FILE: src/sip/sip_dialog_id.cpp
// =============================================================================
#include "sip/sip_dialog_id.h"
#include "common/logger.h"
#include <cstring>

namespace sip_processor {

std::string DialogIdBuilder::build(const sip_t* sip) {
    if (!sip) {
        LOG_ERROR("DialogIdBuilder::build called with null sip");
        return "";
    }
    if (!sip->sip_call_id || !sip->sip_call_id->i_id) {
        LOG_ERROR("DialogIdBuilder::build: missing Call-ID");
        return "";
    }

    std::string call_id = sanitize(sip->sip_call_id->i_id);
    if (call_id.empty()) return "";

    std::string from_tag, to_tag;
    if (sip->sip_from && sip->sip_from->a_tag)
        from_tag = sanitize(sip->sip_from->a_tag);
    if (sip->sip_to && sip->sip_to->a_tag)
        to_tag = sanitize(sip->sip_to->a_tag);

    std::string id = call_id;
    if (!from_tag.empty()) id += ";ft=" + from_tag;
    if (!to_tag.empty())   id += ";tt=" + to_tag;
    return id;
}

std::string DialogIdBuilder::build_from_handle(nua_handle_t* nh) {
    if (!nh) return "";
    char buf[64];
    snprintf(buf, sizeof(buf), "handle:%p", static_cast<void*>(nh));
    return std::string(buf);
}

bool DialogIdBuilder::is_valid(const std::string& dialog_id) {
    return !dialog_id.empty() && dialog_id.size() <= 1024;
}

std::string DialogIdBuilder::sanitize(const char* input, size_t max_len) {
    if (!input) return "";
    std::string result;
    result.reserve(std::min(strlen(input), max_len));
    for (size_t i = 0; input[i] != '\0' && i < max_len; ++i) {
        char c = input[i];
        if (c >= 0x20 && c <= 0x7E && c != ';') result += c;
    }
    return result;
}

} // namespace sip_processor
