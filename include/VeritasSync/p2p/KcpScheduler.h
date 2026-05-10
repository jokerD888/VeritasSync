#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace VeritasSync {

class PeerController;

/**
 * @brief KCP 精确调度器
 *
 * 使用专用线程执行 KCP update，避免与 worker_pool 上的其他任务竞争。
 * KCP 更新包括：驱动 KCP 状态机发送/接收数据、NAT 保活心跳。
 *
 * 专用线程保证 KCP 更新按时执行，不受 SHA256 哈希计算等重型任务影响。
 * juice_send() 在非 io_context 线程上调用，避免 Windows IOCP 死锁检测。
 */
class KcpScheduler {
public:
    using CollectPeersFunc = std::function<std::vector<std::shared_ptr<PeerController>>()>;

    KcpScheduler(boost::asio::io_context& io_context,
                 CollectPeersFunc collect_peers,
                 uint32_t initial_interval_ms = 20);

    ~KcpScheduler();

    void start();
    void stop();

    uint32_t get_interval_ms() const { return m_interval_ms.load(); }

    // 调度间隔上下限
    static constexpr uint32_t INTERVAL_MIN_MS = 1;
    static constexpr uint32_t INTERVAL_MAX_MS = 100;

    // 心跳保活
    static constexpr int KEEPALIVE_INTERVAL_SECONDS = 15;

private:
    void update_thread_func();
    void update_all_kcps();

    boost::asio::io_context& m_io_context;
    CollectPeersFunc m_collect_peers;

    std::atomic<uint32_t> m_interval_ms{20};
    std::atomic<bool> m_running{false};

    // 专用更新线程 + 线程安全停止
    std::thread m_update_thread;
    std::mutex m_stop_mutex;
    std::condition_variable m_stop_cv;
    bool m_stop_requested = false;

    std::chrono::steady_clock::time_point m_last_keepalive_time;
};

}  // namespace VeritasSync