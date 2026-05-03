

#pragma once

// === 标准库（仅头文件声明所需的最小集合）===
#include <memory>
#include <string>
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

// === 子组件 ===
#include "VeritasSync/p2p/KcpScheduler.h"       // KcpScheduler（unique_ptr）
#include "VeritasSync/p2p/PeerRegistry.h"       // PeerRegistry（成员变量值类型）
#include "VeritasSync/p2p/MessageRouter.h"      // MessageRouter（成员变量值类型）
#include "VeritasSync/p2p/BroadcastManager.h"   // BroadcastManager（unique_ptr）

// === 前向声明（替代 #include，减少编译依赖）===
// 以下类型在头文件中仅以指针/引用/智能指针形式出现，无需完整定义
// 完整定义在各自 .cpp 中 #include



namespace VeritasSync {

// === 前向声明 ===
class StateManager;
class TrackerClient;
class PeerController;
class SyncHandler;
class SyncSession;
struct IceConfig;
enum class PeerState;

/// P2P 性能参数与网络配置（传入 P2PManager::create()）
struct P2PManagerConfig {
    // 性能参数
    size_t   chunk_size              = 16384;   // 文件分块大小
    uint32_t kcp_window_size         = 256;     // KCP 窗口大小
    uint32_t kcp_update_interval_ms  = 20;      // KCP 更新间隔（毫秒）

    // STUN/TURN 配置（原 5 个 setter 合并）
    std::string stun_host            = "stun.l.google.com";
    uint16_t    stun_port            = 19302;
    std::string turn_host;
    uint16_t    turn_port            = 3478;
    std::string turn_username;
    std::string turn_password;
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

    virtual ~P2PManager();

    /**
     * @brief 连接到对等点
     * @param peer_addresses 对等点 ID 列表
     * @param force 是否强制重新连接（用于 Tracker 重连后，需要清理旧的无效连接）
     */
    virtual void connect_to_peers(const std::vector<std::string>& peer_addresses, bool force = false);

    // --- 广播方法（委托给 BroadcastManager）---
    virtual void broadcast_current_state();
    virtual void broadcast_file_update(const FileInfo& file_info);
    virtual void broadcast_file_delete(const std::string& relative_path);
    virtual void broadcast_dir_create(const std::string& relative_path);
    virtual void broadcast_dir_delete(const std::string& relative_path);
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
    P2PManager();  // protected 允许测试 Mock 继承
private:
    void init();  // 由 create() 内部调用，配置参数已就绪


    // --- 上层应用逻辑（使用 PeerController）---
    // send_over_kcp 返回bool表示是否至少有一个对等点成功接收
    bool send_over_kcp(const std::string& msg);
    void send_over_kcp_peer(const std::string& msg, PeerController* peer);
    void send_over_kcp_peer_safe(const std::string& msg, const std::string& peer_id);
    void handle_kcp_message(const std::string& msg, PeerController* from_peer);

    // --- 连接管理子方法 ---
    // 创建 PeerController 及其回调
    std::shared_ptr<PeerController> create_peer_controller(const std::string& self_id,
                                                           const std::string& peer_id);
    // Answer 方等待 Offer 超时处理
    void setup_answer_timeout(std::shared_ptr<PeerController> controller,
                              const std::string& peer_id);

    // --- 初始化子方法 ---
    void create_transfer_manager();     // 创建 TransferManager 及其回调
    void create_sync_components();      // 创建 SyncHandler 和 SyncSession
    void start_background_services();   // 启动 IO 线程、定时器、UPnP
    void register_message_handlers();   // 注册所有消息路由（MessageRouter）


    // PeerController 回调处理
    void handle_peer_state_changed(const std::string& peer_id, PeerState state);
    void handle_peer_message(const std::string& peer_id, const std::string& message);

    // 创建 ICE 配置
    IceConfig create_ice_config() const;
    
    // --- 断点续传相关 ---
    static constexpr int GRACEFUL_SHUTDOWN_TIMEOUT_MS = 500;  // 优雅关闭等待超时
    void wait_for_kcp_flush(int timeout_ms = GRACEFUL_SHUTDOWN_TIMEOUT_MS);
    void handle_goodbye(PeerController* from_peer);


    // --- 成员变量 ---
    // m_io_context 所有网络事件的事件循环
    boost::asio::io_context m_io_context;
    std::jthread m_thread;

    TrackerClient* m_tracker_client = nullptr;
    StateManager* m_state_manager = nullptr;
    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // Peer 管理（委托给 PeerRegistry）
    PeerRegistry m_peer_registry;

    std::shared_ptr<CryptoLayer> m_crypto;

    // ---  传输管理器 ---
    std::shared_ptr<TransferManager> m_transfer_manager;

    // --- 同步消息处理器 ---
    std::unique_ptr<SyncHandler> m_sync_handler;

    // --- 同步会话管理器 ---
    std::unique_ptr<SyncSession> m_sync_session;

    // --- STUN/TURN 配置（从 P2PManagerConfig 初始化）---
    std::string m_stun_host;
    uint16_t m_stun_port = 19302;
    std::string m_turn_host;
    uint16_t m_turn_port = 3478;
    std::string m_turn_username;
    std::string m_turn_password;
    // --------------------------

    // --- 文件组装缓冲区清理 ---
    // 清理过期的传输缓冲区
    boost::asio::steady_timer m_cleanup_timer;
    void schedule_cleanup_task();
    void cleanup_stale_buffers();

    // --- UPnP 管理器 ---
    UpnpManager m_upnp;
    // --------------------------

    // --- 子组件 ---
    std::unique_ptr<KcpScheduler> m_kcp_scheduler;          // KCP 自适应更新调度
    MessageRouter m_message_router;                          // 消息类型路由表
    std::unique_ptr<BroadcastManager> m_broadcast_manager;   // 广播与对账
    // --------------------------

    // --- 线程池 ---
    // 用于执行 Hash 计算、文件 IO、压缩加密等耗时操作
    boost::asio::thread_pool m_worker_pool;
    
    // 可配置性能参数
    size_t m_chunk_size = 16384;             // 文件分块大小
    uint32_t m_kcp_window_size = 256;        // KCP 窗口大小
    uint32_t m_kcp_update_interval_ms = 20;  // KCP 更新间隔（毫秒）
};

}  // namespace VeritasSync