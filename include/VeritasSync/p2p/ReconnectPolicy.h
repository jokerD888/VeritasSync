#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace VeritasSync {

/**
 * @brief 重连策略管理器
 * 
 * 管理 Peer 断线后的自动重连，采用指数退避策略：
 *   延迟 = BASE_DELAY * 2^(attempt-1)
 * 
 * 超过最大重试次数后放弃重连。
 */
class ReconnectPolicy {
public:
    /// 执行重连操作的回调（由 P2PManager 提供）
    using ReconnectFunc = std::function<void(const std::string& peer_id)>;
    /// 检查 peer 是否已重新连接的回调
    using IsConnectedFunc = std::function<bool(const std::string& peer_id)>;
    /// 清理旧 peer 的回调
    using CleanupPeerFunc = std::function<void(const std::string& peer_id)>;

    /**
     * @param io_context      事件循环引用
     * @param reconnect_fn    执行重连的回调
     * @param is_connected_fn 检查是否已连接的回调
     * @param cleanup_peer_fn 清理旧 peer 的回调
     * @param max_attempts    最大重试次数，默认 5
     * @param base_delay_ms   基础重连延迟（毫秒），默认 3000ms
     */
    ReconnectPolicy(boost::asio::io_context& io_context,
                    ReconnectFunc reconnect_fn,
                    IsConnectedFunc is_connected_fn,
                    CleanupPeerFunc cleanup_peer_fn,
                    int max_attempts = DEFAULT_MAX_ATTEMPTS,
                    int base_delay_ms = DEFAULT_BASE_DELAY_MS);

    /// 调度一次重连尝试
    void schedule_reconnect(const std::string& peer_id);

    /// 清除指定 peer 的重连计数（连接成功时调用）
    void clear_attempts(const std::string& peer_id);

    /// 获取指定 peer 的当前重试次数
    int get_attempts(const std::string& peer_id) const;

    /// 获取最大重试次数
    int get_max_attempts() const { return m_max_attempts; }

    /// 获取基础重连延迟（毫秒）
    int get_base_delay_ms() const { return m_base_delay_ms; }

    static constexpr int DEFAULT_MAX_ATTEMPTS = 5;
    static constexpr int DEFAULT_BASE_DELAY_MS = 3000;  // 3秒

private:
    boost::asio::io_context& m_io_context;
    ReconnectFunc m_reconnect_fn;
    IsConnectedFunc m_is_connected_fn;
    CleanupPeerFunc m_cleanup_peer_fn;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, int> m_attempts;  // peer_id -> 重试次数

    int m_max_attempts;
    int m_base_delay_ms;
};

}  // namespace VeritasSync
