
// =============================================================================
// FILE: include/sip/sip_stack_manager.h
// =============================================================================
#ifndef SIP_STACK_MANAGER_H
#define SIP_STACK_MANAGER_H
#include "common/config.h"
#include <sofia-sip/nua.h>
#include <sofia-sip/su_wait.h>
#include <thread>
#include <atomic>
namespace sip_processor {
class SipStackManager {
public:
    explicit SipStackManager(const Config& config);
    ~SipStackManager();
    Result start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    nua_t* nua() const { return nua_; }
    su_root_t* root() const { return root_; }
    SipStackManager(const SipStackManager&) = delete;
    SipStackManager& operator=(const SipStackManager&) = delete;
private:
    void run_event_loop();
    Config config_;
    su_root_t* root_ = nullptr;
    su_home_t home_[1];
    nua_t* nua_ = nullptr;
    std::thread sofia_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};
} // namespace sip_processor
#endif