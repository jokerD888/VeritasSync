#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_context.hpp>

#include "VeritasSync/net/IceTransport.h"
#include "VeritasSync/net/KcpSession.h"

namespace VeritasSync {

class P2PManager; // 前向声明

/**
 * @brief Peer 连接状态
 */
enum class PeerState {
    Disconnected,    // 未连接
    Connecting,      // ICE 正在连接
    Connected,       // ICE 已连接，KCP 就绪
    Failed           // 连接失败
};

/**
 * @brief PeerController 回调接口
 * 
 * PeerController 不直接依赖 TrackerClient 或 SyncManager，
 * 通过回调反向通知，保持模块独立性。
 * 
 * 线程安全说明：
 * - 所有回调都在 io_context 线程中触发（通过 post 保证）
 * - 调用方可安全地在回调中调用 PeerController 的方法
 */
struct PeerControllerCallbacks {
    /// 状态变化通知（上层可更新 UI 或触发逻辑）
    std::function<void(PeerState state)> on_state_changed;
    
    /// 需要发送信令（如 ICE 候选或 SDP，通过 TrackerClient 转发）
    /// signal_type: "ice_candidate", "sdp_offer", "sdp_answer", "ice_gathering_done"
    std::function<void(const std::string& signal_type, const std::string& payload)> on_signal_needed;
    
    /// 收到应用层消息（解密后的完整消息）
    std::function<void(const std::string& message)> on_message_received;
};

/**
 * @brief PeerController - 单节点连接控制器
 * 
 * 职责：
 * 1. 组合管理 IceTransport + KcpSession
 * 2. 处理单个 Peer 的连接生命周期
 * 3. 提供消息发送接口
 * 4. 维护同步会话状态
 * 
 * 设计原则：
 * - 每个 PeerController 管理一个 Peer 连接
 * - P2PManager 持有多个 PeerController
 * - 状态变化通过回调通知上层
 * - 所有回调通过 io_context post，保证线程安全
 * 
 * 线程安全：
 * - 内部使用 mutex 保护共享资源
 * - ICE/KCP 回调通过 weak_ptr + post 防止悬垂指针
 * - 公共方法可从任意线程调用
 */
class PeerController : public std::enable_shared_from_this<PeerController> {
public:
    /**
     * @brief 创建 PeerController 实例
     * @param self_id 本地 Peer ID（用于 Conv 计算和角色判断）
     * @param peer_id 对端 Peer ID
     * @param io_context 用于定时器和异步操作
     * @param ice_config ICE 配置
     * @param callbacks 事件回调
     * @return 智能指针，失败返回 nullptr
     */
    static std::shared_ptr<PeerController> create(
        const std::string& self_id,
        const std::string& peer_id,
        boost::asio::io_context& io_context,
        const IceConfig& ice_config,
        PeerControllerCallbacks callbacks);
    
    ~PeerController();
    
    // 禁止拷贝
    PeerController(const PeerController&) = delete;
    PeerController& operator=(const PeerController&) = delete;
    
    // --- 连接控制 ---
    
    /**
     * @brief 作为 Offer 方发起连接
     * 
     * 根据 self_id < peer_id 的规则自动判断角色：
     * - self_id < peer_id: 我们是 Offer 方（控制方）
     * - self_id > peer_id: 我们是 Answer 方（受控方）
     */
    void initiate_connection();
    
    /**
     * @brief 处理收到的信令消息
     * @param signal_type 信令类型 (ice_candidate, sdp_offer, sdp_answer, ice_gathering_done)
     * @param payload 信令内容
     */
    void handle_signaling(const std::string& signal_type, const std::string& payload);
    
    /**
     * @brief 关闭连接并释放资源
     * 
     * 会取消所有定时器，标记为无效，释放 ICE 和 KCP 资源
     */
    void close();
    
    // --- 消息收发 ---
    
    /**
     * @brief 发送应用层消息（通过 KCP）
     * @param data 数据指针
     * @param size 数据大小
     * @return KCP waitsnd 值（用于流控），-1 表示发送失败
     */
    int send_message(const char* data, size_t size);
    
    /**
     * @brief 发送应用层消息 (span 版本)
     */
    int send_message(std::span<const char> data) {
        return send_message(data.data(), data.size());
    }
    
    /**
     * @brief 发送字符串消息
     */
    int send_message(const std::string& message);
    
    // --- KCP 驱动 ---
    
    /**
     * @brief 驱动 KCP 状态机（需定时调用）
     * @param current_ms 当前时间戳（毫秒）
     */
    void update_kcp(uint32_t current_ms);
    
    // --- 状态查询 ---
    
    const std::string& get_self_id() const { return m_self_id; }
    const std::string& get_peer_id() const { return m_peer_id; }
    PeerState get_state() const { return m_state.load(); }
    bool is_connected() const { return m_state.load() == PeerState::Connected; }
    bool is_valid() const { return m_is_valid.load(); }
    
    /// 是否是 Offer 方（self_id < peer_id）
    bool is_offer_side() const { return m_is_offer_side; }
    
    IceConnectionType get_connection_type() const;
    int get_kcp_wait_send() const;
    
    // --- 同步会话状态 ---
    
    std::atomic<uint64_t> sync_session_id{0};
    std::atomic<size_t> expected_file_count{0};
    std::atomic<size_t> expected_dir_count{0};
    std::atomic<size_t> received_file_count{0};
    std::atomic<size_t> received_dir_count{0};
    std::atomic<int64_t> connected_at_ts{0};  // 改为 atomic，线程安全
    
    /// 同步超时定时器
    std::shared_ptr<boost::asio::steady_timer> sync_timeout_timer;
    
    // --- 兼容旧代码 (后续应移除) ---
    
    std::shared_ptr<IceTransport> get_ice_transport() const;
    std::shared_ptr<KcpSession> get_kcp_session() const;

private:
    PeerController(
        const std::string& self_id,
        const std::string& peer_id,
        boost::asio::io_context& io_context,
        PeerControllerCallbacks callbacks);
    
    /// 第一阶段初始化：创建 IceTransport
    bool initialize_ice(const IceConfig& ice_config);
    
    /// 第二阶段初始化：绑定回调（在 shared_ptr 创建后调用）
    void bind_callbacks();
    
    // ICE 回调处理（在 io_context 线程中执行）
    void on_ice_state_changed(IceState state);
    void on_ice_local_candidate(const std::string& candidate);
    void on_ice_gathering_done(const std::string& local_desc);
    void on_ice_data_received(const char* data, size_t size);
    
    // KCP 回调处理
    int on_kcp_output(const char* data, int len);
    void on_kcp_message_received(const std::string& message);
    
    /// 设置 KCP 会话
    void setup_kcp_session();
    
    /// 计算 Conv ID（两端一致）
    uint32_t calculate_conv() const;
    
    std::string m_self_id;   // 本地 ID
    std::string m_peer_id;   // 对端 ID
    bool m_is_offer_side;    // 是否是 Offer 方
    
    boost::asio::io_context& m_io_context;
    PeerControllerCallbacks m_callbacks;
    
    std::shared_ptr<IceTransport> m_ice;
    std::shared_ptr<KcpSession> m_kcp;
    
    std::atomic<PeerState> m_state{PeerState::Disconnected};
    std::atomic<bool> m_is_valid{true};
    std::atomic<bool> m_kcp_initialized{false};  // 防止重复初始化
    
    mutable std::mutex m_mutex;  // 保护 m_ice 和 m_kcp
};

} // namespace VeritasSync
