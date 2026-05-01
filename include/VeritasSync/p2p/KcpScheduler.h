#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace VeritasSync {

class PeerController;

/**
 * @brief KCP 精确调度器
 *
 * 通过 ikcp_check() 查询每个 KCP 会话下次需要 update 的精确时间，
 * 取所有会话的最小值作为下次唤醒时间，避免盲猜频率。
 *
 * 空闲时 KCP 返回较远的时间戳，自然降低频率；
 * 有重传/ACK pending 时返回精确的重传时间，不多不少。
 *
 * 同时负责 NAT 保活心跳。
 */
class KcpScheduler {
public:
    using CollectPeersFunc = std::function<std::vector<std::shared_ptr<PeerController>>()>;

    /**
     * @param io_context    事件循环引用
     * @param collect_peers 获取已连接 Peer 列表的回调
     * @param initial_interval_ms 初始更新间隔（毫秒），默认 20ms
     */
    KcpScheduler(boost::asio::io_context& io_context,
                 CollectPeersFunc collect_peers,
                 uint32_t initial_interval_ms = 20);

    void start();
    void stop();

    uint32_t get_interval_ms() const { return m_interval_ms.load(); }

    // 调度间隔上下限
    static constexpr uint32_t INTERVAL_MIN_MS = 1;    // 最小间隔（防止过于频繁）
    static constexpr uint32_t INTERVAL_MAX_MS = 100;  // 最大间隔（保底唤醒）

    // 心跳保活
    static constexpr int KEEPALIVE_INTERVAL_SECONDS = 15; // NAT 保活心跳间隔

private:
    void schedule_update();
    void update_all_kcps();

    boost::asio::io_context& m_io_context;
    boost::asio::steady_timer m_timer;
    CollectPeersFunc m_collect_peers;

    std::atomic<uint32_t> m_interval_ms{20};
    std::atomic<bool> m_running{false};

    std::chrono::steady_clock::time_point m_last_keepalive_time;
};

}  // namespace VeritasSync
