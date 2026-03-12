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
 * @brief 心跳保活服务
 * 
 * 负责定时向所有已连接的 Peer 发送心跳消息，
 * 维持 NAT 映射，防止连接因空闲而断开。
 * 
 * NAT 映射通常在 30秒-5分钟 超时。
 * libjuice 内置 15 秒 STUN keepalive，
 * 本服务在应用层额外增加心跳作为双保险。
 */
class HeartbeatService {
public:
    /// 获取已连接 Peer 列表的回调
    using CollectPeersFunc = std::function<std::vector<std::shared_ptr<PeerController>>()>;
    /// 向特定 Peer 发送消息的回调
    using SendToPeerFunc = std::function<void(const std::string& msg, PeerController* peer)>;

    /**
     * @param io_context   事件循环引用
     * @param collect_peers 获取已连接 Peer 列表的回调
     * @param send_to_peer  向特定 Peer 发送 JSON 消息的回调
     * @param interval_ms   心跳间隔（毫秒），默认 20000ms
     */
    HeartbeatService(boost::asio::io_context& io_context,
                     CollectPeersFunc collect_peers,
                     SendToPeerFunc send_to_peer,
                     int interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS);

    /// 启动心跳定时器
    void start();

    /// 停止心跳定时器
    void stop();

    /// 处理收到的心跳请求，立即回复 ACK
    void handle_heartbeat(PeerController* from_peer);

    /// 处理收到的心跳 ACK
    void handle_heartbeat_ack(PeerController* from_peer);

    /// 获取心跳间隔（毫秒）
    int get_interval_ms() const { return m_interval_ms; }

    /// 设置心跳间隔（毫秒）
    void set_interval_ms(int ms) { m_interval_ms = ms; }

    static constexpr int DEFAULT_HEARTBEAT_INTERVAL_MS = 20000;  // 20秒

private:
    void schedule_heartbeat();
    void send_heartbeats();

    boost::asio::io_context& m_io_context;
    boost::asio::steady_timer m_timer;
    CollectPeersFunc m_collect_peers;
    SendToPeerFunc m_send_to_peer;
    int m_interval_ms;
    bool m_running = false;
};

}  // namespace VeritasSync
