
// =============================================================================
// FILE: include/sip/sip_dialog_id.h
// =============================================================================
#ifndef SIP_DIALOG_ID_H
#define SIP_DIALOG_ID_H
#include <sofia-sip/sip.h>
#include <sofia-sip/nua.h>
#include <string>

namespace sip_processor {
class DialogIdBuilder {
public:
    static std::string build(const sip_t* sip);
    static std::string build_from_handle(nua_handle_t* nh);
    static bool is_valid(const std::string& dialog_id);
private:
    static std::string sanitize(const char* input, size_t max_len = 256);
};
} // namespace sip_processor
#endif
