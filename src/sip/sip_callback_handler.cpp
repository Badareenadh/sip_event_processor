
// =============================================================================
// FILE: src/sip/sip_callback_handler.cpp
// =============================================================================
#include "sip/sip_callback_handler.h"
#include "sip/sip_event.h"
#include "dispatch/dialog_dispatcher.h"
#include "common/logger.h"
#include <sofia-sip/nua_tag.h>

namespace sip_processor {

DialogDispatcher* SipCallbackHandler::dispatcher_ = nullptr;

void SipCallbackHandler::set_dispatcher(DialogDispatcher* dispatcher) {
    dispatcher_ = dispatcher;
}

bool SipCallbackHandler::should_process(nua_event_t event) {
    switch (event) {
        case nua_i_subscribe: case nua_r_subscribe:
        case nua_i_notify:    case nua_r_notify:
        case nua_i_publish:   case nua_r_publish:
            return true;
        default:
            return false;
    }
}

std::string SipCallbackHandler::extract_tenant_id(const sip_t* sip) {
    if (!sip) return "unknown";
    if (sip->sip_to && sip->sip_to->a_url && sip->sip_to->a_url->url_host)
        return std::string(sip->sip_to->a_url->url_host);
    if (sip->sip_from && sip->sip_from->a_url && sip->sip_from->a_url->url_host)
        return std::string(sip->sip_from->a_url->url_host);
    return "unknown";
}

void SipCallbackHandler::nua_callback(
    nua_event_t event, int status, char const* phrase,
    nua_t*, nua_magic_t*,
    nua_handle_t* nh, nua_hmagic_t*,
    sip_t const* sip, tagi_t[])
{
    if (!should_process(event)) return;

    if (!dispatcher_) {
        LOG_ERROR("NUA callback: dispatcher is null");
        // Respond with 500 to incoming SUBSCRIBE if we can't dispatch
        if (event == nua_i_subscribe && nh) {
            nua_respond(nh, 500, "Internal Server Error",
                        NUTAG_SUBSTATE(nua_substate_terminated), TAG_END());
        }
        return;
    }

    auto sip_event = SipEvent::create_from_sofia(event, status, phrase, nh, sip);
    if (!sip_event) {
        // Respond with 400 to incoming SUBSCRIBE if we can't parse it
        if (event == nua_i_subscribe && nh) {
            nua_respond(nh, 400, "Bad Request",
                        NUTAG_SUBSTATE(nua_substate_terminated), TAG_END());
        }
        return;
    }

    sip_event->tenant_id = extract_tenant_id(sip);

    // Ref the handle for incoming SUBSCRIBE — the worker will own this ref
    // and use it to send responses and NOTIFYs for the dialog lifetime
    if (event == nua_i_subscribe && nh) {
        nua_handle_ref(nh);
    }

    Result r = dispatcher_->dispatch(std::move(sip_event));
    if (r != Result::kOk) {
        LOG_WARN("NUA callback: dispatch failed for %s: %s",
                 nua_event_name(event), result_to_string(r));
        if (event == nua_i_subscribe && nh) {
            // Dispatch failed — respond with 503 and release the ref we just took
            nua_respond(nh, 503, "Service Unavailable",
                        NUTAG_SUBSTATE(nua_substate_terminated), TAG_END());
            nua_handle_unref(nh);
        }
    }
}

} // namespace sip_processor
