

#pragma once

// === 标准库（仅头文件声明所需的最小集合）===
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// === 第三方库（成员变量为值类型，需完整定义）===
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

// === 项目内部（成员变量为值类型或返回值需完整定义）===
#include "VeritasSync/common/Config.h"         // SyncRole, SyncMode, FileInfo
#include "VeritasSync/common/CryptoLayer.h"    // CryptoLayer（成员变量值类型）
#include "VeritasSync/sync/Protocol.h"         // 协议常量
#include "VeritasSync/sync/TransferManager.h"  // TransferManager::SessionStats（返回值类型）
#include "VeritasSync/net/UpnpManager.h"       // UpnpManager（成员变量值类型）

// === A-1 提取的子组件 ===
#include "VeritasSync/p2p/KcpScheduler.h"      // KcpScheduler（unique_ptr）

// === 前向声明（替代 #include，减少编译依赖）===
// 以下类型在头文件中仅以指针/引用/智能指针形式出现，无需完整定义
// 完整定义在各自 .cpp 中 #include



namespace VeritasSync {

// --- ICE 连接类型枚举 ---
enum class ConnectionType {
    None,  // 未连接
    P2P,   // 直连 (host 或 srflx)
    Relay  // 中继 (relay)
};
// -------------------------

// === 前向声明 ===
class StateManager;
class TrackerClient;
class PeerController;
class SyncHandler;
class SyncSession;
struct IceConfig;
enum class PeerState;

/// P2P 性能参数配置（传入 P2PManager::create()）
struct P2PManagerConfig {
    size_t   chunk_size              = 16384;   // 文件分块大小
    uint32_t kcp_window_size         = 256;     // KCP 窗口大小
    uint32_t kcp_update_interval_ms  = 20;      // KCP 更新间隔（毫秒）
};

class P2PManager : public std::enable_shared_from_this<P2PManager> {
public:
    virtual boost::asio::io_context& get_io_context();
    static std::shared_ptr<P2PManager> create(const P2PManagerConfig& config = {});

    void set_encryption_key(const std::string& key_string);

    // --- 依赖注入 ---
    void set_state_manager(StateManager* sm);
    void set_tracker_client(TrackerClient* tc);
    void set_role(SyncRole role);
    void set_mode(SyncMode mode);
    void set_stun_config(std::string host, uint16_t port);
    void set_turn_config(std::string host, uint16_t port, std::string username, std::string password);
    void set_extra_stun_servers(std::vector<std::pair<std::string, uint16_t>> servers, bool enable);

    virtual ~P2PManager();

    /**
     * @brief 连接到对等点
     * @param peer_addresses 对等点 ID 列表
     * @param force 是否强制重新连接（用于 Tracker 重连后，需要清理旧的无效连接）
     */
    virtual void connect_to_peers(const std::vector<std::string>& peer_addresses, bool force = false);

    // --- 广播方法 ---
    virtual void broadcast_current_state();
    virtual void broadcast_file_update(const FileInfo& file_info);
    virtual void broadcast_file_delete(const std::string& relative_path);
    virtual void broadcast_dir_create(const std::string& relative_path);
    virtual void broadcast_dir_delete(const std::string& relative_path);
    
    // --- 批量广播方法 (阶段1优化) ---
    virtual void broadcast_file_updates_batch(const std::vector<FileInfo>& files);
    virtual void broadcast_file_deletes_batch(const std::vector<std::string>& paths);
    virtual void broadcast_dir_changes_batch(const std::vector<std::string>& creates, 
                                             const std::vector<std::string>& deletes);

    // --- 由 TrackerClient 调用 ---
    virtual void handle_signaling_message(const std::string& from_peer_id, const std::string& message_type,
                                          const std::string& payload);
    virtual void handle_peer_leave(const std::string& peer_id);
    
    // --- 断点续传相关 ---
    
    /**
     * @brief 优雅关闭：广播 goodbye 消息并等待发送完成
     * 
     * 在程序正常退出时调用，确保对端能区分"主动退出"和"掉线"
     */
    void shutdown_gracefully();

    std::vector<TransferStatus> get_active_transfers();

    TransferManager::SessionStats get_transfer_stats();

    // --- 对等点状态查询 (Web UI 用) ---
    struct PeerInfo {
        std::string peer_id;           // 对端 ID
        std::string state;             // 连接状态字符串: connected, connecting, disconnected, failed
        std::string connection_type;   // 连接类型: direct, relay, unknown
        int64_t connected_since;       // 连接建立时间 (epoch秒)，未连接为 0
    };
    std::vector<PeerInfo> get_peers_info();

protected:
    P2PManager();
    void init();  // 由 create() 内部调用，配置参数已就绪

    // --- KCP 集成（已提取到 KcpScheduler）---

    // --- Peer 访问辅助方法（A-3: 消除重复的锁+遍历/查找模式）---
    // 收集所有已连接的 PeerController（在读锁内拷贝，锁外安全使用）
    std::vector<std::shared_ptr<PeerController>> collect_connected_peers() const;

    /// 向多个 peer 发送消息，带 KCP 流控背压（在 worker 线程中调用）
    /// @return 是否全部发送成功
    bool send_to_peers_with_flow_control(
        const std::vector<std::shared_ptr<PeerController>>& peers,
        const std::string& packet);

    // 按 peer_id 查找 PeerController（读锁内拷贝 shared_ptr）
    std::shared_ptr<PeerController> find_peer(const std::string& peer_id) const;

    // --- 上层应用逻辑（使用 PeerController）---
    // LOGIC-003: send_over_kcp 返回bool表示是否至少有一个对等点成功接收
    bool send_over_kcp(const std::string& msg);
    void send_over_kcp_peer(const std::string& msg, PeerController* peer);
    void send_over_kcp_peer_safe(const std::string& msg, const std::string& peer_id);
    void handle_kcp_message(const std::string& msg, PeerController* from_peer);

    // --- A-2: connect_to_peers 拆分子方法 ---
    // 检查已存在的 peer 是否可复用，返回 true 表示应跳过该 peer
    bool try_reuse_existing_peer(const std::string& peer_id, bool force);
    // 创建 PeerController 及其回调
    std::shared_ptr<PeerController> create_peer_controller(const std::string& self_id,
                                                           const std::string& peer_id);
    // Answer 方等待 Offer 超时处理
    void setup_answer_timeout(std::shared_ptr<PeerController> controller,
                              const std::string& peer_id);

    // --- A-2: init 拆分子方法 ---
    void create_transfer_manager();     // 创建 TransferManager 及其回调
    void create_sync_components();      // 创建 SyncHandler 和 SyncSession
    void start_background_services();   // 启动 IO 线程、定时器、UPnP

    // --- A-2: handle_kcp_message 拆分子方法 ---
    // JSON 消息分发路由
    void dispatch_json_message(const std::string& json_msg_type,
                               nlohmann::json& json_payload,
                               PeerController* from_peer,
                               bool can_receive);

    // --- 消息处理器（已迁移到 SyncHandler）---

    // --- 同步会话管理（已迁移到 SyncSession）---

    // 【重构】新增：PeerController 回调处理
    void handle_peer_state_changed(const std::string& peer_id, PeerState state);
    void handle_peer_message(const std::string& peer_id, const std::string& message);

    // 【中继回退】ICE 失败时通过 Tracker 中继数据
    void attempt_relay_fallback(const std::string& peer_id);
    void handle_relay_data(const std::string& from_peer_id, const uint8_t* data, size_t len);
    
    // 【重构】新增：创建 ICE 配置
    IceConfig create_ice_config() const;
    
    // --- 断点续传相关 ---
    void broadcast_goodbye();
    static constexpr int GRACEFUL_SHUTDOWN_TIMEOUT_MS = 500;  // 优雅关闭等待超时
    void wait_for_kcp_flush(int timeout_ms = GRACEFUL_SHUTDOWN_TIMEOUT_MS);
    void handle_goodbye(PeerController* from_peer);


    // --- UPnP 辅助函数（已迁移到 UpnpManager）---

    // --- 成员变量 ---
    // m_io_context 所有网络事件的事件循环
    boost::asio::io_context m_io_context;
    std::jthread m_thread;

    TrackerClient* m_tracker_client = nullptr;
    StateManager* m_state_manager = nullptr;
    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // 【重构】新的 Peer 管理方式
    // 使用 shared_mutex: 读操作(查找)可并行，写操作(增删)互斥
    std::unordered_map<std::string, std::shared_ptr<PeerController>> m_peers;
    mutable std::shared_mutex m_peers_mutex;

    CryptoLayer m_crypto;

    // ---  传输管理器 ---
    std::shared_ptr<TransferManager> m_transfer_manager;

    // --- 同步消息处理器 ---
    std::unique_ptr<SyncHandler> m_sync_handler;

    // --- 同步会话管理器 ---
    std::unique_ptr<SyncSession> m_sync_session;

    // --- STUN 服务器配置 ---
    std::string m_stun_host = "stun.l.google.com";  // 默认公共STUN
    uint16_t m_stun_port = 19302;
    // --------------------------

    std::string m_turn_host;
    uint16_t m_turn_port = 3478;
    std::string m_turn_username;
    std::string m_turn_password;

    // --- Multi-STUN Probing 配置 ---
    std::vector<std::pair<std::string, uint16_t>> m_extra_stun_servers;
    bool m_enable_multi_stun_probing = false;
    // --------------------------

    // --- 文件组装缓冲区清理 ---
    // 清理过期的传输缓冲区
    boost::asio::steady_timer m_cleanup_timer;
    void schedule_cleanup_task();
    void cleanup_stale_buffers();

    // --- UPnP 管理器 ---
    UpnpManager m_upnp;
    // --------------------------

    // --- A-1: 提取的子组件 ---
    std::unique_ptr<KcpScheduler> m_kcp_scheduler;          // KCP 自适应更新调度
    // --------------------------

    // --- 线程池 ---
    // 用于执行 Hash 计算、文件 IO、压缩加密等耗时操作
    boost::asio::thread_pool m_worker_pool;
    
    // 【修复 #7】可配置性能参数
    size_t m_chunk_size = 16384;             // 文件分块大小
    uint32_t m_kcp_window_size = 256;        // KCP 窗口大小
    uint32_t m_kcp_update_interval_ms = 20;  // KCP 更新间隔（毫秒）
};

}  // namespace VeritasSync