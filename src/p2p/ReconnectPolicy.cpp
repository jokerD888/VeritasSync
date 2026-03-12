#include "VeritasSync/p2p/ReconnectPolicy.h"

#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

ReconnectPolicy::ReconnectPolicy(boost::asio::io_context& io_context,
                                 ReconnectFunc reconnect_fn,
                                 IsConnectedFunc is_connected_fn,
                                 CleanupPeerFunc cleanup_peer_fn,
                                 int max_attempts,
                                 int base_delay_ms)
    : m_io_context(io_context),
      m_reconnect_fn(std::move(reconnect_fn)),
      m_is_connected_fn(std::move(is_connected_fn)),
      m_cleanup_peer_fn(std::move(cleanup_peer_fn)),
      m_max_attempts(max_attempts),
      m_base_delay_ms(base_delay_ms) {
}

void ReconnectPolicy::schedule_reconnect(const std::string& peer_id) {
    int attempt_count;
    int delay_ms;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        int& count = m_attempts[peer_id];
        count++;
        attempt_count = count;

        if (attempt_count > m_max_attempts) {
            g_logger->error("[ICE] 对等点 {} 重连次数超过上限 ({})，放弃重连。",
                            peer_id, m_max_attempts);
            m_attempts.erase(peer_id);
            return;
        }

        delay_ms = m_base_delay_ms * (1 << (attempt_count - 1));
    }

    g_logger->info("[ICE] 将在 {}ms 后尝试第 {} 次重连 (对等点: {})",
                   delay_ms, attempt_count, peer_id);

    auto timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    timer->expires_after(std::chrono::milliseconds(delay_ms));
    timer->async_wait([this, peer_id, timer](const boost::system::error_code& ec) {
        if (ec) return;

        // 检查是否已经重新连接
        if (m_is_connected_fn(peer_id)) {
            g_logger->info("[ICE] 对等点 {} 已重新连接，取消重连。", peer_id);
            return;
        }

        // 清理旧的 controller
        m_cleanup_peer_fn(peer_id);

        // 重新连接
        m_reconnect_fn(peer_id);
    });
}

void ReconnectPolicy::clear_attempts(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_attempts.erase(peer_id);
}

int ReconnectPolicy::get_attempts(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_attempts.find(peer_id);
    if (it != m_attempts.end()) {
        return it->second;
    }
    return 0;
}

}  // namespace VeritasSync
