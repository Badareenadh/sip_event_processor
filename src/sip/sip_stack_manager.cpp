
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

} // namespace sip_processor
