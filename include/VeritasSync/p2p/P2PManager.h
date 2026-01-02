
#pragma once

#include <array>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <unordered_map>
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



/*
┌─────────────────────────────────────────────────────────────────────────────┐
│                              用户界面层 (UI)                                  │
│                        (未来可能的 GUI / CLI)                                 │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │ 调用
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           应用编排层 (App)                                   │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │  负责：启动各模块、注入依赖、协调生命周期                              │   │
│   │  核心：设置 StateManager、P2PManager、TrackerClient 之间的关系        │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
└─────────┬───────────────────────────┬───────────────────────────┬───────────┘
          │                           │                           │
          ▼                           ▼                           ▼
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────────┐
│   StateManager      │   │    P2PManager       │   │    TrackerClient        │
│   (本地状态管理)     │   │   (P2P连接中枢)      │   │   (信令服务器客户端)     │
└─────────────────────┘   └─────────────────────┘   └─────────────────────────┘
          │                         │ ▲                           │
          │                         │ │                           │
          │   ┌─────────────────────┘ │                           │
          │   │                       │                           │
          ▼   ▼                       │                           │
┌─────────────────────┐               │                           │
│   TransferManager   │◄──────────────┘                           │
│   (文件传输管理)     │                                           │
└─────────────────────┘                                           │
          │                                                       │
          │                                                       │
          ▼                                                       ▼
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────────┐
│   SyncManager       │   │   PeerController    │   │   Tracker Server        │
│   (同步算法)         │   │   (单个P2P连接)      │   │   (云端信令中继)         │
└─────────────────────┘   └─────────────────────┘   └─────────────────────────┘
          │                         │ │
          ▼                         │ │
┌─────────────────────┐             │ │
│   FileFilter        │             │ │
│   (文件过滤)         │             ▼ ▼
└─────────────────────┘   ┌─────────────────────┐   ┌─────────────────────────┐
          │               │   KcpSession        │   │   libjuice (ICE)        │
          ▼               │   (可靠传输)         │   │   (NAT穿透)              │
┌─────────────────────┐   └─────────────────────┘   └─────────────────────────┘
│   Database          │
│   (SQLite持久化)     │
└─────────────────────┘


                    ┌─────────────────────────────────────────┐
                    │           通用工具层                     │
                    │  ┌────────────┐  ┌────────────────────┐ │
                    │  │ CryptoLayer│  │ Hashing / Logger   │ │
                    │  │ (加解密)    │  │ EncodingUtils      │ │
                    │  └────────────┘  └────────────────────┘ │
                    └─────────────────────────────────────────┘

二、各模块职责详解
层级 1：核心管理层
模块	                职责	                                一句话总结
StateManager	        管理本地文件系统状态（扫描、监控、记录修改）	"我知道本地有什么文件"
P2PManager	        管理所有对等点连接，协调消息收发和同步逻辑	        "我负责与其他节点通信"
TrackerClient	    与信令服务器通信，用于发现对等点和交换 ICE 候选	"我帮你找到其他节点在哪里"

层级 2：传输与同步层    
模块	                职责	                                一句话总结
TransferManager	        负责文件分块传输：分块、压缩、加密、发送、接收、组装	"我负责把大文件切成小块传过去"
SyncManager	        纯算法模块：比较本地和远程状态，决定要做什么操作	"我告诉你哪些文件需要下载/删除/忽略"
PeerController	    封装单个对等点的 ICE 连接 + KCP 会话	        "我是与某一个节点的连接管道"

层级 3：底层支撑层
模块	                职责	                                一句话总结
KcpSession	            封装 KCP 协议，提供可靠 UDP 传输	        "我让 UDP 变得可靠"
FileFilter	            根据规则过滤不需要同步的文件/目录	        "我告诉你哪些文件不用管"
Database	            SQLite 持久化，存储同步历史、配置等	        "我把重要数据存到硬盘上"
CryptoLayer	            AES-GCM 加解密	                        "我保护你的数据安全"
libjuice	            ICE 协议实现（NAT 穿透）	                    "我帮你穿透 NAT"


三、模块间的调用关系
3.1 依赖注入关系
App 启动时：
    ┌──────────────────────────────────────────────────────────┐
    │  1. 创建 StateManager                                     │
    │     - 依赖: Database, FileFilter                         │
    │                                                          │
    │  2. 创建 P2PManager                                       │
    │     - 依赖: CryptoLayer (内置)                            │
    │     - 注入: StateManager (set_state_manager)             │
    │     - 注入: TrackerClient (set_tracker_client)           │
    │                                                          │
    │  3. 创建 TrackerClient                                    │
    │     - 注入: P2PManager (用于回调信令消息)                  │
    │                                                          │
    │  4. 设置角色和模式                                         │
    │     - P2PManager.set_role(Source/Destination)            │
    │     - P2PManager.set_mode(OneWay/BiDirectional)          │
    └──────────────────────────────────────────────────────────┘
3.2 数据流动路径
场景 A：Source 发送文件更新给 Destination
┌─────────────┐     文件变化      ┌─────────────────┐
│  文件系统    │  ─────────────→  │  StateManager   │
└─────────────┘       监控        └────────┬────────┘
                                           │ 通知变化
                                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                        P2PManager                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ 1. broadcast_file_update(file_info)                     │   │
│  │ 2. send_over_kcp() → 加密 → 发送给所有连接的 Peer        │   │
│  └─────────────────────────────────────────────────────────┘   │
└────────────────────────────┬────────────────────────────────────┘
                             │ 通过 KCP/ICE
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PeerController                             │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ send_message() → KcpSession.send() → libjuice.send()   │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                             │
                             │ 网络传输 (UDP)
                             ▼
           ─────────────────────────────────────────
                        互联网
           ─────────────────────────────────────────
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│              Destination 端的 P2PManager                        │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ handle_kcp_message() → 解密 → 路由到 handle_file_update │   │
│  └─────────────────────────────────────────────────────────┘   │
└────────────────────────────┬────────────────────────────────────┘
                             │ 需要下载？
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                      TransferManager                            │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ 发送 request_file 消息请求文件内容                       │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘


场景 B：Destination 请求文件，Source 发送
Destination                                              Source
    │                                                       │
    │─── request_file {path: "a.txt"} ───────────────────→ │
    │                                                       │
    │                                    P2PManager.handle_kcp_message()
    │                                    ↓ 路由到
    │                                    TransferManager.queue_upload()
    │                                    ↓
    │                                    读取文件 → 分块 → 压缩 → 加密
    │                                                       │
    │ ←─────────────── chunk_1 {data, index, total} ────────│
    │ ←─────────────── chunk_2 {data, index, total} ────────│
    │ ←─────────────── chunk_N {data, index, total} ────────│
    │                                                       │
    │  TransferManager.handle_chunk()                       │
    │  ↓ 解密 → 解压 → 写入临时文件                           │
    │  ↓ 所有块收齐后，重命名为目标文件                        │
    │                                                       │
    │  StateManager.record_sync_success()                   │
    │                                                       │


场景 C：ICE 连接建立流程
节点 A (Offer方)                 Tracker Server              节点 B (Answer方)
    │                                │                           │
    │  join_room(room_id)           │                           │
    │───────────────────────────────→                           │
    │                                │←── join_room(room_id) ────│
    │                                │                           │
    │←─ peer_list [B] ───────────────│                           │
    │                                │─── peer_list [A] ─────────→
    │                                │                           │
    │  P2PManager.connect_to_peers([B])                         │
    │  ↓                                                        │
    │  创建 PeerController(A, B)                                 │
    │  ↓  A > B → A 是 Offer 方                                  │
    │  controller->initiate_connection()                        │
    │  ↓                                                        │
    │  生成 ICE Offer (SDP)                                      │
    │                                │                           │
    │──── ice_offer {SDP} ──────────→│                           │
    │                                │── ice_offer {SDP} ───────→│
    │                                │                           │
    │                                │         PeerController.handle_signaling()
    │                                │         ↓ 设置远端 SDP
    │                                │         ↓ 生成 ICE Answer
    │                                │                           │
    │                                │←─ ice_answer {SDP} ───────│
    │←── ice_answer {SDP} ──────────│                           │
    │                                │                           │
    │  PeerController.handle_signaling()                        │
    │  ↓ 设置远端 SDP                                            │
    │                                │                           │
    │←───────────── ice_candidate (多次交换) ───────────────────→│
    │                                │                           │
    │  ICE 连接建立成功 (P2P 或 Relay)                            │
    │  ↓                                                        │
    │  KcpSession 初始化                                         │
    │  ↓                                                        │
    │←════════════ KCP 加密通信开始 ════════════════════════════→│


五、核心模块关系图（简化版）
                    ┌───────────────────┐
                    │   TrackerClient   │ ←───── 信令服务器
                    │   (发现 + 信令)    │
                    └─────────┬─────────┘
                              │ on_peer_list / on_signaling
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                        P2PManager                            │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  管理所有 PeerController                                 │ │
│  │  消息加密/解密/路由                                       │ │
│  │  同步会话协调 (sync_begin/sync_ack)                      │ │
│  └─────────────────────────────────────────────────────────┘ │
│                         │            │                       │
│           ┌─────────────┴────┐  ┌────┴─────────────┐        │
│           ▼                  ▼  ▼                  │        │
│  ┌────────────────┐  ┌────────────────────────┐   │        │
│  │ PeerController │  │   TransferManager      │   │        │
│  │ × N 个连接      │  │   (分块传输)            │   │        │
│  │ ┌────────────┐ │  └────────────────────────┘   │        │
│  │ │ KcpSession │ │                               │        │
│  │ │ (可靠UDP)   │ │                               │        │
│  │ └────────────┘ │                               │        │
│  │ ┌────────────┐ │                               │        │
│  │ │ libjuice   │ │                               │        │
│  │ │ (ICE穿透)   │ │                               │        │
│  │ └────────────┘ │                               │        │
│  └────────────────┘                               │        │
└───────────────────────────────────────────────────┼────────┘
                                                    │
                                                    ▼
                                         ┌─────────────────┐
                                         │  StateManager   │
                                         │  (本地状态管理)  │
                                         │  ┌───────────┐  │
                                         │  │FileFilter │  │
                                         │  └───────────┘  │
                                         │  ┌───────────┐  │
                                         │  │ Database  │  │
                                         │  └───────────┘  │
                                         └─────────────────┘

                    */



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
    // m_io_context 所有网络事件的事件循环
    boost::asio::io_context m_io_context;
    std::jthread m_thread;
    // 定时驱动 KCP 协议的 update()
    boost::asio::steady_timer m_kcp_update_timer;

    TrackerClient* m_tracker_client = nullptr;
    StateManager* m_state_manager = nullptr;
    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // 【重构】新的 Peer 管理方式
    // 使用 shared_mutex: 读操作(查找)可并行，写操作(增删)互斥
    std::unordered_map<std::string, std::shared_ptr<PeerController>> m_peers;
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
    // 清理过期的传输缓冲区
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
    std::unordered_map<std::string, int> m_reconnect_attempts;  // peer_id -> 重试次数
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_last_reconnect_time;  // peer_id -> 上次重连时间
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;  // 最大重试次数
    static constexpr int BASE_RECONNECT_DELAY_MS = 3000;  // 基础重连延迟 3秒
    void schedule_reconnect(const std::string& peer_id);
    // ---------------------------------

    // --- 线程池 ---
    // 用于执行 Hash 计算、文件 IO、压缩加密等耗时操作
    boost::asio::thread_pool m_worker_pool;
};

}  // namespace VeritasSync