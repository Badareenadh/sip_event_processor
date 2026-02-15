

// =============================================================================
// FILE: src/sip/sip_event.cpp
// =============================================================================
#include "sip/sip_event.h"
#include "sip/sip_dialog_id.h"
#include "subscription/subscription_type.h"
#include "common/logger.h"
#include <cstring>

namespace sip_processor {

std::atomic<EventId> SipEvent::id_counter_{0};

EventId SipEvent::next_id() {
    return id_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
}

static SipEventCategory categorize_nua_event(nua_event_t event) {
    switch (event) {
        case nua_i_subscribe: case nua_r_subscribe: return SipEventCategory::kSubscribe;
        case nua_i_notify:    case nua_r_notify:    return SipEventCategory::kNotify;
        case nua_i_publish:   case nua_r_publish:   return SipEventCategory::kPublish;
        default: return SipEventCategory::kUnknown;
    }
}

static SipDirection determine_direction(nua_event_t event) {
    switch (event) {
        case nua_i_subscribe: case nua_i_notify: case nua_i_publish:
            return SipDirection::kIncoming;
        default:
            return SipDirection::kOutgoing;
    }
}

static std::string safe_copy(const char* str) { return str ? std::string(str) : ""; }
static std::string safe_copy_n(const char* str, size_t max) {
    if (!str) return "";
    return std::string(str, strnlen(str, max));
}

std::unique_ptr<SipEvent> SipEvent::create_from_sofia(
    nua_event_t event, int status, const char* phrase,
    nua_handle_t* nh, const sip_t* sip)
{
    auto ev = std::make_unique<SipEvent>();
    ev->id         = next_id();
    ev->nua_event  = event;
    ev->status     = status;
    ev->phrase     = safe_copy_n(phrase, 256);
    ev->direction  = determine_direction(event);
    ev->category   = categorize_nua_event(event);
    ev->source     = SipEventSource::kSipStack;
    ev->created_at = Clock::now();
    ev->nua_handle = nh;

    if (sip) {
        ev->dialog_id = DialogIdBuilder::build(sip);

        if (sip->sip_call_id && sip->sip_call_id->i_id)
            ev->call_id = safe_copy_n(sip->sip_call_id->i_id, 256);

        if (sip->sip_from) {
            if (sip->sip_from->a_url) {
                auto* u = sip->sip_from->a_url;
                if (u->url_user && u->url_host)
                    ev->from_uri = std::string("sip:") + u->url_user + "@" + u->url_host;
                else if (u->url_host)
                    ev->from_uri = std::string("sip:") + u->url_host;
            }
            ev->from_tag = safe_copy_n(sip->sip_from->a_tag, 128);
        }

        if (sip->sip_to) {
            if (sip->sip_to->a_url) {
                auto* u = sip->sip_to->a_url;
                if (u->url_user && u->url_host)
                    ev->to_uri = std::string("sip:") + u->url_user + "@" + u->url_host;
                else if (u->url_host)
                    ev->to_uri = std::string("sip:") + u->url_host;
            }
            ev->to_tag = safe_copy_n(sip->sip_to->a_tag, 128);
        }

        if (sip->sip_event && sip->sip_event->o_type) {
            ev->event_header = safe_copy_n(sip->sip_event->o_type, 128);
            ev->sub_type = parse_subscription_type(sip->sip_event->o_type);
        }

        if (sip->sip_cseq) ev->cseq = sip->sip_cseq->cs_seq;
        if (sip->sip_expires) ev->expires = sip->sip_expires->ex_delta;

        if (sip->sip_content_type && sip->sip_content_type->c_type)
            ev->content_type = safe_copy_n(sip->sip_content_type->c_type, 256);

        if (sip->sip_payload && sip->sip_payload->pl_data) {
            size_t body_len = sip->sip_payload->pl_len;
            if (body_len > 0 && body_len < 65536)
                ev->body = std::string(sip->sip_payload->pl_data, body_len);
            else if (body_len >= 65536) {
                LOG_WARN("Event %lu: body too large (%zu), truncating", ev->id, body_len);
                ev->body = std::string(sip->sip_payload->pl_data, 65536);
            }
        }

        if (sip->sip_subscription_state) {
            if (sip->sip_subscription_state->ss_substate)
                ev->subscription_state = safe_copy_n(sip->sip_subscription_state->ss_substate, 64);
            if (sip->sip_subscription_state->ss_reason)
                ev->termination_reason = safe_copy_n(sip->sip_subscription_state->ss_reason, 64);
        }
    } else if (nh) {
        ev->dialog_id = DialogIdBuilder::build_from_handle(nh);
    }

    if (ev->dialog_id.empty()) {
        LOG_WARN("Event %lu: could not build dialog ID", ev->id);
        return nullptr;
    }

    return ev;
}

std::unique_ptr<SipEvent> SipEvent::create_presence_trigger(
    const std::string& dialog_id,
    const std::string& tenant_id,
    const std::string& presence_call_id,
    const std::string& caller_uri,
    const std::string& callee_uri,
    const std::string& blf_state,
    const std::string& direction,
    const std::string& dialog_info_xml_body)
{
    auto ev = std::make_unique<SipEvent>();
    ev->id                 = next_id();
    ev->dialog_id          = dialog_id;
    ev->tenant_id          = tenant_id;
    ev->category           = SipEventCategory::kPresenceTrigger;
    ev->source             = SipEventSource::kPresenceFeed;
    ev->sub_type           = SubscriptionType::kBLF;
    ev->direction          = SipDirection::kIncoming;
    ev->presence_call_id   = presence_call_id;
    ev->presence_caller_uri = caller_uri;
    ev->presence_callee_uri = callee_uri;
    ev->presence_state     = blf_state;
    ev->presence_direction = direction;
    ev->content_type       = "application/dialog-info+xml";
    ev->body               = dialog_info_xml_body;
    ev->created_at         = Clock::now();
    ev->nua_handle         = nullptr;  // Will be looked up by the worker

    LOG_TRACE("Presence trigger event %lu created: dialog=%s state=%s callee=%s",
              ev->id, dialog_id.c_str(), blf_state.c_str(), callee_uri.c_str());

    return ev;
}

} // namespace sip_processor

