#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace VeritasSync {

class PeerController;

/**
 * @brief KCP 自适应更新调度器
 * 
 * 定时调用所有已连接 Peer 的 KCP update()，
 * 并根据活跃度动态调整更新频率：
 *   - 活跃期：  5ms  （有待发送数据）
 *   - 近期活跃：10ms （5秒内有过活跃）
 *   - 空闲期：  100ms（超过 5秒无活跃）
 */
class KcpScheduler {
public:
    /// 获取已连接 Peer 列表的回调
    using CollectPeersFunc = std::function<std::vector<std::shared_ptr<PeerController>>()>;

    /**
     * @param io_context    事件循环引用
     * @param collect_peers 获取已连接 Peer 列表的回调
     * @param initial_interval_ms 初始更新间隔（毫秒），默认 20ms
     */
    KcpScheduler(boost::asio::io_context& io_context,
                 CollectPeersFunc collect_peers,
                 uint32_t initial_interval_ms = 20);

    /// 启动 KCP 更新定时器
    void start();

    /// 停止 KCP 更新定时器
    void stop();

    /// 获取当前更新间隔（毫秒）
    uint32_t get_interval_ms() const { return m_interval_ms; }

    // 自适应频率参数
    static constexpr uint32_t INTERVAL_ACTIVE_MS  = 5;    // 活跃时更新间隔
    static constexpr uint32_t INTERVAL_RECENT_MS  = 10;   // 近期活跃更新间隔
    static constexpr uint32_t INTERVAL_IDLE_MS    = 100;  // 空闲时更新间隔
    static constexpr int      IDLE_THRESHOLD_SECONDS = 5; // 空闲判定阈值（秒）
    static constexpr int      KEEPALIVE_INTERVAL_SECONDS = 15; // 心跳间隔（秒）

private:
    void schedule_update();
    void update_all_kcps();

    boost::asio::io_context& m_io_context;
    boost::asio::steady_timer m_timer;
    CollectPeersFunc m_collect_peers;

    uint32_t m_interval_ms = 20;  // 默认 20ms，动态调整
    std::chrono::steady_clock::time_point m_last_data_time;
    std::chrono::steady_clock::time_point m_last_keepalive_time;
    bool m_running = false;
};

}  // namespace VeritasSync
