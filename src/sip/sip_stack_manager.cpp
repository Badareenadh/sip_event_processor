
// =============================================================================
// FILE: src/sip/sip_stack_manager.cpp
// =============================================================================
#include "sip/sip_stack_manager.h"
#include "sip/sip_callback_handler.h"
#include "common/logger.h"
#include <sofia-sip/su.h>
#include <sofia-sip/su_alloc.h>
#include <sofia-sip/nua_tag.h>
#include <sofia-sip/sip_tag.h>
#include <cstring>
#include <cstdio>

namespace sip_processor {

SipStackManager::SipStackManager(const Config& config) : config_(config) {
    su_home_init(home_);
}

SipStackManager::~SipStackManager() {
    stop();
    su_home_deinit(home_);
}

Result SipStackManager::start() {
    if (running_.load(std::memory_order_acquire)) return Result::kAlreadyExists;

    su_init();
    root_ = su_root_create(nullptr);
    if (!root_) { LOG_FATAL("Failed to create Sofia root"); return Result::kError; }

    nua_ = nua_create(root_,
                      SipCallbackHandler::nua_callback, nullptr,
                      NUTAG_URL(config_.sip_bind_url.c_str()),
                      NUTAG_USER_AGENT(config_.sip_user_agent.c_str()),
                      NUTAG_ALLOW("SUBSCRIBE, NOTIFY, PUBLISH"),
                      TAG_END());

    if (!nua_) {
        LOG_FATAL("Failed to create NUA on %s", config_.sip_bind_url.c_str());
        su_root_destroy(root_); root_ = nullptr;
        return Result::kError;
    }

    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    sofia_thread_ = std::thread(&SipStackManager::run_event_loop, this);

    LOG_INFO("SIP stack started on %s", config_.sip_bind_url.c_str());
    return Result::kOk;
}

void SipStackManager::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    LOG_INFO("Stopping SIP stack...");
    stop_requested_.store(true, std::memory_order_release);

    if (root_) su_root_break(root_);
    if (sofia_thread_.joinable()) sofia_thread_.join();

    if (nua_) {
        nua_shutdown(nua_);
        for (int i = 0; i < 50; ++i) su_root_step(root_, 100);
        nua_destroy(nua_); nua_ = nullptr;
    }
    if (root_) { su_root_destroy(root_); root_ = nullptr; }

    su_deinit();
    running_.store(false, std::memory_order_release);
    LOG_INFO("SIP stack stopped");
}

void SipStackManager::run_event_loop() {
    LOG_INFO("Sofia event loop thread started");
    while (!stop_requested_.load(std::memory_order_acquire)) {
        su_root_step(root_, 100);
    }
    LOG_INFO("Sofia event loop thread exiting");
}

void SipStackManager::respond_to_subscribe(nua_handle_t* nh, int status,
                                            const char* phrase, uint32_t expires) {
    if (!nh) {
        LOG_WARN("respond_to_subscribe: null handle");
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        LOG_WARN("respond_to_subscribe: stack not running");
        return;
    }

    int substate;
    if (status >= 200 && status < 300)
        substate = nua_substate_active;
    else
        substate = nua_substate_terminated;

    char expires_str[32];
    snprintf(expires_str, sizeof(expires_str), "%u", expires);

    LOG_DEBUG("SIP: responding %d %s to SUBSCRIBE (expires=%u)", status, phrase, expires);

    nua_respond(nh, status, phrase,
                NUTAG_SUBSTATE(substate),
                SIPTAG_EXPIRES_STR(expires_str),
                TAG_END());
}

void SipStackManager::send_notify(nua_handle_t* nh, const char* event_type,
                                   const char* content_type, const char* body,
                                   const char* subscription_state_str) {
    if (!nh) {
        LOG_WARN("send_notify: null handle");
        return;
    }
    if (!running_.load(std::memory_order_acquire)) {
        LOG_WARN("send_notify: stack not running");
        return;
    }

    int substate = nua_substate_active;
    if (subscription_state_str) {
        if (strcmp(subscription_state_str, "terminated") == 0)
            substate = nua_substate_terminated;
        else if (strcmp(subscription_state_str, "pending") == 0)
            substate = nua_substate_pending;
    }

    LOG_DEBUG("SIP: sending NOTIFY event=%s state=%s body_len=%zu",
              event_type ? event_type : "(null)",
              subscription_state_str ? subscription_state_str : "active",
              body ? strlen(body) : 0);

    nua_notify(nh,
               NUTAG_SUBSTATE(substate),
               SIPTAG_EVENT_STR(event_type),
               SIPTAG_CONTENT_TYPE_STR(content_type),
               SIPTAG_PAYLOAD_STR(body),
               TAG_END());
}

} // namespace sip_processor
