
// =============================================================================
// FILE: src/presence/presence_failover_manager.cpp
// =============================================================================
#include "presence/presence_failover_manager.h"
#include "common/logger.h"
#include <random>
#include <algorithm>

namespace sip_processor {

PresenceFailoverManager::PresenceFailoverManager(const Config& config)
    : config_(config)
{
    for (const auto& ep : config.presence_servers) {
        ServerHealth h;
        h.endpoint = ep;
        servers_.push_back(std::move(h));
    }
    LOG_INFO("FailoverManager: initialized with %zu servers", servers_.size());
}

int PresenceFailoverManager::find_server(const PresenceServerEndpoint& ep) const {
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (servers_[i].endpoint.host == ep.host && servers_[i].endpoint.port == ep.port)
            return static_cast<int>(i);
    }
    return -1;
}

bool PresenceFailoverManager::is_in_cooldown(const ServerHealth& h) const {
    if (h.cooldown_until == TimePoint{}) return false;
    return Clock::now() < h.cooldown_until;
}

PresenceServerEndpoint PresenceFailoverManager::get_next_server() {
    std::lock_guard<std::mutex> lk(mu_);

    if (servers_.empty()) return {};

    int idx = -1;
    switch (config_.presence_failover_strategy) {
        case FailoverStrategy::kRoundRobin: idx = select_round_robin(); break;
        case FailoverStrategy::kPriority:   idx = select_priority(); break;
        case FailoverStrategy::kRandom:     idx = select_random(); break;
    }

    if (idx < 0) {
        // All servers in cooldown — pick the one whose cooldown expires soonest
        TimePoint earliest = TimePoint::max();
        for (size_t i = 0; i < servers_.size(); ++i) {
            if (servers_[i].cooldown_until < earliest) {
                earliest = servers_[i].cooldown_until;
                idx = static_cast<int>(i);
            }
        }
        if (idx >= 0) {
            LOG_WARN("FailoverManager: all servers in cooldown, forcing %s:%d",
                     servers_[idx].endpoint.host.c_str(), servers_[idx].endpoint.port);
        }
    }

    if (idx < 0) return {};

    servers_[idx].last_attempt = Clock::now();
    LOG_INFO("FailoverManager: selected server %s:%d (failures=%d)",
             servers_[idx].endpoint.host.c_str(), servers_[idx].endpoint.port,
             servers_[idx].consecutive_failures);

    return servers_[idx].endpoint;
}

int PresenceFailoverManager::select_round_robin() {
    size_t n = servers_.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (round_robin_index_ + i) % n;
        if (!is_in_cooldown(servers_[idx]) && servers_[idx].is_healthy) {
            round_robin_index_ = (idx + 1) % n;
            return static_cast<int>(idx);
        }
    }
    // Try unhealthy but not in cooldown
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (round_robin_index_ + i) % n;
        if (!is_in_cooldown(servers_[idx])) {
            round_robin_index_ = (idx + 1) % n;
            return static_cast<int>(idx);
        }
    }
    return -1;
}

int PresenceFailoverManager::select_priority() {
    int best = -1;
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (is_in_cooldown(servers_[i])) continue;
        if (best < 0 || servers_[i].endpoint.priority < servers_[best].endpoint.priority)
            best = static_cast<int>(i);
    }
    return best;
}

int PresenceFailoverManager::select_random() {
    std::vector<int> available;
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (!is_in_cooldown(servers_[i]) && servers_[i].is_healthy)
            available.push_back(static_cast<int>(i));
    }
    if (available.empty()) {
        for (size_t i = 0; i < servers_.size(); ++i)
            if (!is_in_cooldown(servers_[i])) available.push_back(static_cast<int>(i));
    }
    if (available.empty()) return -1;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, available.size() - 1);
    return available[dist(rng)];
}

void PresenceFailoverManager::report_success(const PresenceServerEndpoint& ep) {
    std::lock_guard<std::mutex> lk(mu_);
    int idx = find_server(ep);
    if (idx < 0) return;

    auto& h = servers_[idx];
    h.is_healthy = true;
    h.consecutive_failures = 0;
    h.total_successes++;
    h.last_success = Clock::now();
    h.cooldown_until = {};

    LOG_INFO("FailoverManager: %s:%d reported healthy (total_ok=%d)",
             ep.host.c_str(), ep.port, h.total_successes);
}

void PresenceFailoverManager::report_failure(const PresenceServerEndpoint& ep,
                                              const std::string& reason) {
    std::lock_guard<std::mutex> lk(mu_);
    int idx = find_server(ep);
    if (idx < 0) return;

    auto& h = servers_[idx];
    h.consecutive_failures++;
    h.total_failures++;
    h.last_failure = Clock::now();

    // Progressive cooldown: double cooldown for each consecutive failure, capped
    int multiplier = std::min(h.consecutive_failures, 5);
    Seconds cooldown = Seconds(config_.presence_server_cooldown.count() * multiplier);
    h.cooldown_until = Clock::now() + cooldown;

    if (h.consecutive_failures >= 3) h.is_healthy = false;

    LOG_WARN("FailoverManager: %s:%d failure #%d (reason=%s, cooldown=%lds)",
             ep.host.c_str(), ep.port, h.consecutive_failures,
             reason.c_str(), cooldown.count());
}

void PresenceFailoverManager::mark_unhealthy(const PresenceServerEndpoint& ep) {
    std::lock_guard<std::mutex> lk(mu_);
    int idx = find_server(ep);
    if (idx >= 0) servers_[idx].is_healthy = false;
}

void PresenceFailoverManager::mark_healthy(const PresenceServerEndpoint& ep) {
    std::lock_guard<std::mutex> lk(mu_);
    int idx = find_server(ep);
    if (idx >= 0) { servers_[idx].is_healthy = true; servers_[idx].cooldown_until = {}; }
}

std::vector<PresenceFailoverManager::ServerHealth>
PresenceFailoverManager::get_all_health() const {
    std::lock_guard<std::mutex> lk(mu_);
    return servers_;
}

bool PresenceFailoverManager::any_server_available() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& h : servers_)
        if (!is_in_cooldown(h)) return true;
    return false;
}

size_t PresenceFailoverManager::healthy_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t c = 0;
    for (const auto& h : servers_) if (h.is_healthy) c++;
    return c;
}

void PresenceFailoverManager::reset_all() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& h : servers_) {
        h.is_healthy = true;
        h.consecutive_failures = 0;
        h.cooldown_until = {};
    }
}

} // namespace sip_processor
