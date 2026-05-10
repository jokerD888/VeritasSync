#include "VeritasSync/p2p/KcpScheduler.h"
#include "VeritasSync/net/KcpProto.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/common/Logger.h"

#include <algorithm>

namespace VeritasSync {

KcpScheduler::KcpScheduler(boost::asio::io_context& io_context,
                           CollectPeersFunc collect_peers,
                           uint32_t initial_interval_ms)
    : m_io_context(io_context),
      m_collect_peers(std::move(collect_peers)),
      m_interval_ms(initial_interval_ms),
      m_last_keepalive_time(std::chrono::steady_clock::now()) {
}

KcpScheduler::~KcpScheduler() {
    stop();
}

void KcpScheduler::start() {
    if (m_running.exchange(true)) return;
    m_update_thread = std::thread(&KcpScheduler::update_thread_func, this);
}

void KcpScheduler::stop() {
    if (!m_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        m_stop_requested = true;
    }
    m_stop_cv.notify_one();
    if (m_update_thread.joinable()) {
        m_update_thread.join();
    }
}

void KcpScheduler::update_thread_func() {
    while (m_running.load()) {
        auto start_time = std::chrono::steady_clock::now();

        if (m_running.load()) {
            try {
                update_all_kcps();
            } catch (const std::exception& e) {
                if (g_logger) g_logger->error("[KcpScheduler] update_all_kcps 异常: {}", e.what());
            } catch (...) {
                if (g_logger) g_logger->error("[KcpScheduler] update_all_kcps 未知异常");
            }
        }

        // 计算下次唤醒时间
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto interval = std::chrono::milliseconds(m_interval_ms.load());
        auto remaining = interval - elapsed;
        if (remaining < std::chrono::milliseconds(1)) {
            remaining = std::chrono::milliseconds(1);
        }

        // 等待剩余时间或被 stop() 唤醒
        std::unique_lock<std::mutex> lock(m_stop_mutex);
        m_stop_cv.wait_for(lock, remaining, [this]() { return m_stop_requested; });
        if (m_stop_requested) break;
    }
}

void KcpScheduler::update_all_kcps() {
    auto now = std::chrono::steady_clock::now();
    auto current_time_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());

    // 收集 controller 列表（线程安全）
    auto controllers = m_collect_peers();

    if (controllers.empty()) {
        return;
    }

    // 驱动所有 KCP 并查询下次唤醒时间
    uint32_t next_wakeup = current_time_ms + INTERVAL_MAX_MS;

    for (auto& ctrl : controllers) {
        if (!ctrl->is_valid()) continue;

        ctrl->update_kcp(current_time_ms);

        uint32_t kcp_next = ctrl->check_kcp(current_time_ms);
        next_wakeup = std::min(next_wakeup, kcp_next);
    }

    // 计算下次定时器间隔
    uint32_t elapsed_since_now = next_wakeup > current_time_ms
        ? next_wakeup - current_time_ms : 0;
    uint32_t interval = std::clamp(elapsed_since_now, INTERVAL_MIN_MS, INTERVAL_MAX_MS);
    m_interval_ms.store(interval);

    // NAT 保活心跳
    auto since_last_keepalive = now - m_last_keepalive_time;
    if (since_last_keepalive >= std::chrono::seconds(KEEPALIVE_INTERVAL_SECONDS)) {
        m_last_keepalive_time = now;
        std::string ping_msg(1, static_cast<char>(MSG_TYPE_PING));
        for (auto& ctrl : controllers) {
            if (ctrl->is_valid() && ctrl->is_connected()) {
                ctrl->send_message(ping_msg);
            }
        }
    }
}

}  // namespace VeritasSync