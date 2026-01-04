
#pragma once

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/common/CryptoLayer.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/sync/TransferManager.h"
#include "VeritasSync/p2p/PeerController.h"

// --- miniupnpc 头文件 ---
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
// --------------------------

namespace VeritasSync {

// --- ICE 连接类型枚举 ---
enum class ConnectionType {
    None,  // 未连接
    P2P,   // 直连 (host 或 srflx)
    Relay  // 中继 (relay)
};
// -------------------------

enum class SyncRole { Source, Destination };

class StateManager;
class P2PManager;
class TrackerClient;

class P2PManager : public std::enable_shared_from_this<P2PManager> {
public:
    virtual boost::asio::io_context& get_io_context();
    static std::shared_ptr<P2PManager> create();

    void set_encryption_key(const std::string& key_string);

    // --- 依赖注入 ---
    void set_state_manager(StateManager* sm);
    void set_tracker_client(TrackerClient* tc);
    void set_role(SyncRole role);
    void set_mode(SyncMode mode) { m_mode = mode; }
    void set_stun_config(std::string host, uint16_t port);
    void set_turn_config(std::string host, uint16_t port, std::string username, std::string password);

    virtual ~P2PManager();

    virtual void connect_to_peers(const std::vector<std::string>& peer_addresses);

    // --- 广播方法 ---
    virtual void broadcast_current_state();
    virtual void broadcast_file_update(const FileInfo& file_info);
    virtual void broadcast_file_delete(const std::string& relative_path);
    virtual void broadcast_dir_create(const std::string& relative_path);
    virtual void broadcast_dir_delete(const std::string& relative_path);

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

protected:
    P2PManager();
    void init();

    // --- KCP 集成 ---
    void schedule_kcp_update();
    void update_all_kcps();

    // --- 上层应用逻辑（使用 PeerController）---
    void send_over_kcp(const std::string& msg);
    void send_over_kcp_peer(const std::string& msg, PeerController* peer);
    void send_over_kcp_peer_safe(const std::string& msg, const std::string& peer_id);
    void handle_kcp_message(const std::string& msg, PeerController* from_peer);

    // --- 消息处理器（使用 PeerController）---
    void handle_share_state(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_update(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_delete(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_request(const nlohmann::json& payload, PeerController* from_peer);

    void handle_dir_create(const nlohmann::json& payload, PeerController* from_peer);
    void handle_dir_delete(const nlohmann::json& payload, PeerController* from_peer);

    // --- 同步会话管理（使用 PeerController）---
    void handle_sync_begin(const nlohmann::json& payload, PeerController* from_peer);
    void handle_sync_ack(const nlohmann::json& payload, PeerController* from_peer);
    void send_sync_begin(PeerController* peer, uint64_t session_id, size_t file_count, size_t dir_count);
    void send_sync_ack(PeerController* peer, uint64_t session_id, size_t received_files, size_t received_dirs);
    void perform_flood_sync(std::shared_ptr<PeerController> controller, uint64_t session_id);
    // -----------------------

    // 【重构】新增：PeerController 回调处理
    void handle_peer_state_changed(const std::string& peer_id, PeerState state);
    void handle_peer_message(const std::string& peer_id, const std::string& message);
    
    // 【重构】新增：创建 ICE 配置
    IceConfig create_ice_config() const;
    
    // --- 断点续传相关 ---
    void broadcast_goodbye();
    void wait_for_kcp_flush(int timeout_ms = 500);
    void handle_goodbye(PeerController* from_peer);

    // 【重构】移除旧的 libjuice 回调
#if 0
    // --- libjuice 回调 (C 风格) ---
    static void on_juice_state_changed(juice_agent_t* agent, juice_state_t state, void* user_ptr);
    static void on_juice_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr);
    static void on_juice_gathering_done(juice_agent_t* agent, void* user_ptr);
    static void on_juice_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr);

    // --- libjuice 回调的 C++ 处理器 ---
    void handle_juice_state_changed(juice_agent_t* agent, juice_state_t state);
    void handle_juice_candidate(juice_agent_t* agent, const char* sdp);
    void handle_juice_gathering_done(juice_agent_t* agent);
    void handle_juice_recv(juice_agent_t* agent, const char* data, size_t size);
#endif

    // --- UPnP 辅助函数 ---
    void init_upnp();
    std::string rewrite_candidate(const std::string& sdp_candidate);

    // --- 成员变量 ---
    boost::asio::io_context m_io_context;
    std::jthread m_thread;
    boost::asio::steady_timer m_kcp_update_timer;

    TrackerClient* m_tracker_client = nullptr;
    StateManager* m_state_manager = nullptr;
    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // 【重构】新的 Peer 管理方式
    // 使用 shared_mutex: 读操作(查找)可并行，写操作(增删)互斥
    std::map<std::string, std::shared_ptr<PeerController>> m_peers;
    mutable std::shared_mutex m_peers_mutex;
    
    // 【重构】注释旧的双向映射
#if 0
    // --- 关键：双向映射 ---
    std::map<juice_agent_t*, std::shared_ptr<PeerContext>> m_peers_by_agent;
    std::map<std::string, std::shared_ptr<PeerContext>> m_peers_by_id;
    // -----------------------
#endif

    CryptoLayer m_crypto;

    // ---  传输管理器 ---
    std::shared_ptr<TransferManager> m_transfer_manager;

    // --- STUN 服务器配置 ---
    std::string m_stun_host = "stun.l.google.com";  // 默认公共STUN
    uint16_t m_stun_port = 19302;
    // --------------------------

    std::string m_turn_host;
    uint16_t m_turn_port = 3478;
    std::string m_turn_username;
    std::string m_turn_password;
    // 【重构】移除 libjuice 特定结构
    // juice_turn_server_t m_turn_server_config;

    // --- KCP更新频率自适应 ---
    uint32_t m_kcp_update_interval_ms = 20;  // 默认20ms，在10-100ms之间动态调整
    std::chrono::steady_clock::time_point m_last_data_time;

    // --- 文件组装缓冲区清理 ---
    boost::asio::steady_timer m_cleanup_timer;
    void schedule_cleanup_task();
    void cleanup_stale_buffers();

    // --- UPnP 成员变量 ---
    std::mutex m_upnp_mutex;
    bool m_upnp_available = false;
    char m_upnp_lan_addr[64] = {0};
    std::string m_upnp_public_ip;
    struct UPNPUrls m_upnp_urls;
    struct IGDdatas m_upnp_data;
    // --------------------------

    // --- ICE FAILED 自动重连机制 ---
    std::mutex m_reconnect_mutex;
    std::map<std::string, int> m_reconnect_attempts;  // peer_id -> 重试次数
    std::map<std::string, std::chrono::steady_clock::time_point> m_last_reconnect_time;  // peer_id -> 上次重连时间
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;  // 最大重试次数
    static constexpr int BASE_RECONNECT_DELAY_MS = 3000;  // 基础重连延迟 3秒
    void schedule_reconnect(const std::string& peer_id);
    // ---------------------------------

    // --- 线程池 ---
    // 用于执行 Hash 计算、文件 IO、压缩加密等耗时操作
    boost::asio::thread_pool m_worker_pool;
};

}  // namespace VeritasSync