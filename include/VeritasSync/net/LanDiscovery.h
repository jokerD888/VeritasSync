#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

namespace VeritasSync {

/// 通过 LAN 发现的远端对等点信息
struct LanPeerInfo {
    std::string device_id;       ///< 对端设备 ID
    std::string lan_ip;          ///< 对端 LAN IP
    uint16_t    signaling_port;  ///< TCP 信令端口
    uint16_t    kcp_port;        ///< KCP 直连 UDP 端口（0 表示未设）
    int64_t     last_seen_ms;    ///< 最后心跳时间戳
};

/// LAN 发现模块回调
struct LanDiscoveryCallbacks {
    /// 发现新的对等点时调用（或原有对等点 IP/端口变化时）
    std::function<void(const LanPeerInfo& peer)> on_peer_discovered;
    /// 对等点超时离线时调用
    std::function<void(const std::string& device_id)> on_peer_lost;
    /// 收到 LAN 信令消息时调用（signal_type: "ice_candidate" / "sdp_offer" / "sdp_answer" / "ice_gathering_done"）
    std::function<void(const std::string& from_device_id,
                       const std::string& signal_type,
                       const std::string& payload)> on_signaling_received;
};

/**
 * @brief LAN 多播发现 + TCP 信令通道
 *
 * 职责：
 * 1. 定时发 UDP 组播心跳广播本端存在
 * 2. 监听同组心跳，匹配 sync_key 后触发 on_peer_discovered
 * 3. 提供 TCP 信令通道用于 ICE 候选交换（替代 Tracker 中转）
 * 4. 超时检测（30 秒无心跳标记离线）
 *
 * 线程安全：所有回调通过 io_context::post 派发
 */
class LanDiscovery : public std::enable_shared_from_this<LanDiscovery> {
public:
    /// 创建实例（不启动，需调用 start()）
    /// @param kcp_udp_port 本端 KCP 直连 UDP 端口（用于心跳广播）
    static std::shared_ptr<LanDiscovery> create(
        boost::asio::io_context& io_context,
        const std::string& device_id,
        const std::string& sync_key,
        LanDiscoveryCallbacks callbacks,
        uint16_t multicast_port = 9989,
        const std::string& multicast_group = "239.255.0.100",
        uint16_t kcp_udp_port = 0);

    ~LanDiscovery();

    /// 启动发现（开启心跳发送 + 多播监听 + TCP 信令服务）
    void start();

    /// 停止所有网络活动
    void stop();

    /// 向指定 LAN 对端发送 ICE 信令（通过 TCP 直连通道）
    /// signal_type: "ice_candidate", "sdp_offer", "sdp_answer", "ice_gathering_done"
    /// 返回 true 表示 TCP 连接建立成功并开始发送
    bool send_signaling(const std::string& peer_device_id,
                        const std::string& signal_type,
                        const std::string& payload);

    /// 获取当前所有活跃的 LAN 对等点
    std::vector<LanPeerInfo> get_peers() const;

    /// 获取 TCP 信令服务监听端口（用于外部传入）
    uint16_t get_signaling_port() const { return m_signaling_port; }

    /// 刷新指定对等点的最后活跃时间（有数据收发时调用，防止大流量时心跳超时误判）
    void refresh_peer(const std::string& peer_device_id);

private:
    LanDiscovery(boost::asio::io_context& io_context,
                 const std::string& device_id,
                 const std::string& sync_key,
                 LanDiscoveryCallbacks callbacks,
                 uint16_t multicast_port,
                 const std::string& multicast_group,
                 uint16_t kcp_udp_port);

    // --- UDP 多播 ---
    void start_multicast_receive();
    void do_multicast_receive();
    void start_heartbeat();
    void do_send_heartbeat();
    void handle_heartbeat(const std::string& data, const boost::asio::ip::udp::endpoint& sender);

    // --- TCP 信令服务 ---
    void start_tcp_signaling_server();
    void do_tcp_accept();
    void handle_tcp_session(std::shared_ptr<boost::asio::ip::tcp::socket> sock);

    // --- TCP 信令客户端 ---
    struct SignalingChannel {
        std::string peer_id;
        std::shared_ptr<boost::asio::ip::tcp::socket> socket;
        boost::asio::streambuf read_buf;
        int64_t connected_ms;
    };
    void connect_signaling(const std::string& peer_id, const std::string& ip, uint16_t port);
    void do_signaling_write(std::shared_ptr<SignalingChannel> ch, std::string msg);
    void do_signaling_read(std::shared_ptr<SignalingChannel> ch);

    // --- 超时检测 ---
    void start_timeout_check();
    void check_timeouts();

    // --- 工具 ---
    static std::string make_heartbeat_payload(const std::string& device_id,
                                              const std::string& sync_key,
                                              uint16_t signaling_port,
                                              uint16_t kcp_port);
    static bool parse_heartbeat(const std::string& payload,
                                std::string& out_device_id,
                                std::string& out_sync_key,
                                uint16_t& out_signaling_port,
                                uint16_t& out_kcp_port);

    // --- 成员 ---
    boost::asio::io_context& m_io_context;
    std::string m_device_id;
    std::string m_sync_key;
    LanDiscoveryCallbacks m_callbacks;
    uint16_t m_multicast_port;
    std::string m_multicast_group;

    // UDP 多播
    boost::asio::ip::udp::socket m_udp_socket;
    boost::asio::ip::udp::endpoint m_multicast_endpoint;
    boost::asio::ip::udp::endpoint m_remote_endpoint;
    std::array<char, 1024> m_recv_buffer;

    // 本端 KCP 直连 UDP 端口（在心跳中广播，由 P2PManager 管理共享 socket）
    uint16_t m_kcp_udp_port = 0;

    // TCP 信令服务
    boost::asio::ip::tcp::acceptor m_tcp_acceptor;
    uint16_t m_signaling_port = 0;

    // 定时器
    boost::asio::steady_timer m_heartbeat_timer;
    boost::asio::steady_timer m_timeout_timer;

    // 已知对等点（device_id → LanPeerInfo + 时间戳）
    mutable std::mutex m_peers_mutex;
    struct TrackedPeer {
        LanPeerInfo info;
        std::shared_ptr<SignalingChannel> signaling_ch;
    };
    std::unordered_map<std::string, TrackedPeer> m_peers;

    // 全局最后活跃时间（任一 peer 有数据收发的时间）
    std::atomic<int64_t> m_last_activity_time{0};

    std::atomic<bool> m_running{false};
    static constexpr int HEARTBEAT_INTERVAL_MS = 2000;
    static constexpr int HEARTBEAT_IDLE_THRESHOLD_MS = 5000;  // 超过 5 秒无数据才发心跳
    static constexpr int PEER_TIMEOUT_MS = 120000;  // 2 分钟无任何活动才算离线
    static constexpr int TIMEOUT_CHECK_INTERVAL_MS = 5000;
};

} // namespace VeritasSync
