#include "VeritasSync/p2p/KcpScheduler.h"

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/PeerController.h"

namespace VeritasSync {

KcpScheduler::KcpScheduler(boost::asio::io_context& io_context,
                           CollectPeersFunc collect_peers)
    : m_io_context(io_context),
      m_timer(io_context),
      m_collect_peers(std::move(collect_peers)),
      m_last_data_time(std::chrono::steady_clock::now()) {
}

void KcpScheduler::start() {
    m_running = true;
    m_last_data_time = std::chrono::steady_clock::now();
    schedule_update();
}

void KcpScheduler::stop() {
    m_running = false;
    m_timer.cancel();
}

void KcpScheduler::schedule_update() {
    if (!m_running) return;

    m_timer.expires_after(std::chrono::milliseconds(m_interval_ms));
    m_timer.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && m_running) {
            update_all_kcps();
        }
    });
}

void KcpScheduler::update_all_kcps() {
    auto current_time_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    bool has_activity = false;

    // 【关键修复】先在锁内复制需要更新的 controller 列表
    // 然后在锁外调用 update_kcp()，避免死锁
    // 死锁场景：
    //   update_all_kcps() 持有 m_peers_mutex
    //   -> controller->update_kcp()
    //   -> m_kcp->receive() 触发回调
    //   -> handle_peer_message() 尝试获取 m_peers_mutex → 死锁！
    auto controllers_to_update = m_collect_peers();

    // 在锁外更新 KCP（回调可以安全地获取锁）
    for (auto& controller : controllers_to_update) {
        if (controller->is_valid()) {
            controller->update_kcp(current_time_ms);
            if (controller->get_kcp_wait_send() > 0) {
                has_activity = true;
            }
        }
    }

    // 自适应更新频率
    if (has_activity) {
        m_last_data_time = std::chrono::steady_clock::now();
        m_interval_ms = INTERVAL_ACTIVE_MS;
    } else {
        auto idle_duration = std::chrono::steady_clock::now() - m_last_data_time;
        if (idle_duration > std::chrono::seconds(IDLE_THRESHOLD_SECONDS)) {
            m_interval_ms = INTERVAL_IDLE_MS;
        } else {
            m_interval_ms = INTERVAL_RECENT_MS;
        }
    }

    schedule_update();
}

}  // namespace VeritasSync
