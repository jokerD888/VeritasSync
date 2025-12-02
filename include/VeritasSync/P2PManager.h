
#pragma once
#include <ikcp.h>
#include <juice/juice.h>

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/Config.h"
#include "VeritasSync/CryptoLayer.h"
#include "VeritasSync/Protocol.h"
#include "VeritasSync/TransferManager.h"

// --- miniupnpc 头文件 ---
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
// --------------------------

#include "VeritasSync/Protocol.h"

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
class TrackerClient;  // 前向声明

struct PeerContext {
    std::string peer_id;
    juice_agent_t* agent = nullptr;
    ikcpcb* kcp = nullptr;
    std::shared_ptr<P2PManager> p2p_manager_ptr;

    // --- 连接类型跟踪 ---
    ConnectionType current_type = ConnectionType::None;
    // --------------------

    PeerContext(std::string id, juice_agent_t* ag, std::shared_ptr<P2PManager> manager_ptr);
    ~PeerContext();
    void setup_kcp(uint32_t conv);
};

class StateManager;

class P2PManager : public std::enable_shared_from_this<P2PManager> {
public:
    boost::asio::io_context& get_io_context();
    // --- create 不再需要参数 ---
    static std::shared_ptr<P2PManager> create();

    void set_encryption_key(const std::string& key_string);

    // --- 依赖注入 ---
    void set_state_manager(StateManager* sm);
    void set_tracker_client(TrackerClient* tc);
    void set_role(SyncRole role);
    void set_mode(SyncMode mode) { m_mode = mode; }
    void set_stun_config(std::string host, uint16_t port);
    void set_turn_config(std::string host, uint16_t port, std::string username, std::string password);

    static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user);

    ~P2PManager();

    void connect_to_peers(const std::vector<std::string>& peer_addresses);

    // --- 广播方法 ---
    void broadcast_current_state();
    void broadcast_file_update(const FileInfo& file_info);
    void broadcast_file_delete(const std::string& relative_path);
    void broadcast_dir_create(const std::string& relative_path);
    void broadcast_dir_delete(const std::string& relative_path);

    // --- 由 TrackerClient 调用 ---
    void handle_signaling_message(const std::string& from_peer_id, const std::string& message_type,
                                  const std::string& payload);
    void handle_peer_leave(const std::string& peer_id);

    std::vector<TransferStatus> get_active_transfers();

private:
    P2PManager();
    void init();

    // --- KCP 集成 ---
    void schedule_kcp_update();
    void update_all_kcps();

    // --- 上层应用逻辑 ---
    void send_over_kcp(const std::string& msg);
    void send_over_kcp_peer(const std::string& msg, PeerContext* peer);
    void handle_kcp_message(const std::string& msg, PeerContext* from_peer);

    // --- 消息处理器 ---
    void handle_share_state(const nlohmann::json& payload, PeerContext* from_peer);
    void handle_file_update(const nlohmann::json& payload, PeerContext* from_peer);
    void handle_file_delete(const nlohmann::json& payload, PeerContext* from_peer);
    void handle_file_request(const nlohmann::json& payload, PeerContext* from_peer);

    void handle_dir_create(const nlohmann::json& payload);
    void handle_dir_delete(const nlohmann::json& payload, PeerContext* from_peer);

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

    // --- 关键：双向映射 ---
    std::map<juice_agent_t*, std::shared_ptr<PeerContext>> m_peers_by_agent;
    std::map<std::string, std::shared_ptr<PeerContext>> m_peers_by_id;
    std::mutex m_peers_mutex;
    // -----------------------

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
    juice_turn_server_t m_turn_server_config;

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

    // --- 线程池 ---
    // 用于执行 Hash 计算、文件 IO、压缩加密等耗时操作
    boost::asio::thread_pool m_worker_pool;
};

}  // namespace VeritasSync
