
// =============================================================================
// FILE: include/sip/sip_callback_handler.h
// =============================================================================
#ifndef SIP_CALLBACK_HANDLER_H
#define SIP_CALLBACK_HANDLER_H
#include <sofia-sip/nua.h>
#include <string>
namespace sip_processor {
class DialogDispatcher;
class SipCallbackHandler {
public:
    static void set_dispatcher(DialogDispatcher* dispatcher);
    static void nua_callback(nua_event_t event, int status, char const* phrase,
        nua_t* nua, nua_magic_t* magic, nua_handle_t* nh, nua_hmagic_t* hmagic,
        sip_t const* sip, tagi_t tags[]);
private:
    static DialogDispatcher* dispatcher_;
    static bool should_process(nua_event_t event);
    static std::string extract_tenant_id(const sip_t* sip);
};
} // namespace sip_processor
#endif
