#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "VeritasSync/p2p/P2PManager.h"

// ============================================================
// 【重构完成】从 PeerContext 迁移到 PeerController
// 日期：2026-01-01
// ============================================================

#include <httplib.h>

#include <algorithm>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/SyncManager.h"
#include "VeritasSync/p2p/TrackerClient.h"
#define BUFFERSIZE 32768
#include <b64/decode.h>
#include <b64/encode.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

bool can_broadcast(SyncRole role, SyncMode mode) {
    if (role == SyncRole::Source) return true;
    if (mode == SyncMode::BiDirectional) return true;
    return false;
}

uint16_t read_uint16(const char*& data, size_t& len) {
    if (len < sizeof(uint16_t)) return 0;
    uint16_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_short(net_val);
}

uint32_t read_uint32(const char*& data, size_t& len) {
    if (len < sizeof(uint32_t)) return 0;
    uint32_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_long(net_val);
}

static const uint8_t MSG_TYPE_JSON = 0x01;
static const uint8_t MSG_TYPE_BINARY_CHUNK = 0x02;

// ═══════════════════════════════════════════════════════════════
// 【重构】新增辅助函数
// ═══════════════════════════════════════════════════════════════

IceConfig P2PManager::create_ice_config() const {
    IceConfig config;
    config.stun_host = m_stun_host;
    config.stun_port = m_stun_port;
    if (!m_turn_host.empty()) {
        config.turn_host = m_turn_host;
        config.turn_port = m_turn_port;
        config.turn_username = m_turn_username;
        config.turn_password = m_turn_password;
    }
    return config;
}

// ═══════════════════════════════════════════════════════════════
// 基础设置
// ═══════════════════════════════════════════════════════════════

void P2PManager::set_encryption_key(const std::string& key_string) { 
    m_crypto.set_key(key_string); 
}

static const int GCM_IV_LEN = 12;
static const int GCM_TAG_LEN = 16;

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

// ═══════════════════════════════════════════════════════════════
// 广播方法
// ═══════════════════════════════════════════════════════════════

void P2PManager::broadcast_current_state() {
    if (!can_broadcast(m_role, m_mode)) return;
    if (!m_state_manager) return;

    boost::asio::post(m_worker_pool, [self = shared_from_this()]() {
        self->m_state_manager->scan_directory();
        std::string json_state = self->m_state_manager->get_state_as_json_string();

        std::string json_packet;
        json_packet.push_back(MSG_TYPE_JSON);
        json_packet.append(json_state);

        std::string encrypted_msg = self->m_crypto.encrypt(json_packet);

        if (encrypted_msg.empty()) {
            g_logger->error("[Worker] 加密失败，放弃广播。");
            return;
        }

        // 【重构】使用 m_peers 替代 m_peers_by_agent
        boost::asio::post(self->m_io_context, [self, encrypted_msg = std::move(encrypted_msg)]() {
            std::shared_lock<std::shared_mutex> lock(self->m_peers_mutex);  // 读操作
            int sent_count = 0;
            for (auto& [peer_id, controller] : self->m_peers) {
                if (controller->is_connected()) {
                    controller->send_message(encrypted_msg);
                    sent_count++;
                }
            }
            if (sent_count > 0) {
                g_logger->info("[P2P] (Source) 广播状态完成 (发送给 {} 个对等点)", sent_count);
            }
        });
    });
}

void P2PManager::broadcast_file_update(const FileInfo& file_info) {
    if (!can_broadcast(m_role, m_mode)) return;
    g_logger->info("[P2P] (Source) 广播增量更新: {}", file_info.path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
    msg[Protocol::MSG_PAYLOAD] = file_info;
    send_over_kcp(msg.dump());
}

void P2PManager::broadcast_file_delete(const std::string& relative_path) {
    if (!can_broadcast(m_role, m_mode)) return;
    g_logger->info("[P2P] (Source) 广播增量删除: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}

void P2PManager::broadcast_dir_create(const std::string& relative_path) {
    if (!can_broadcast(m_role, m_mode)) return;
    g_logger->info("[P2P] (Source) 广播增量目录创建: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_CREATE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}

void P2PManager::broadcast_dir_delete(const std::string& relative_path) {
    if (!can_broadcast(m_role, m_mode)) return;
    g_logger->info("[P2P] (Source) 广播增量目录删除: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_DELETE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}

// ═══════════════════════════════════════════════════════════════
// 静态工厂与构造/析构
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<P2PManager> P2PManager::create() {
    struct P2PManagerMaker : public P2PManager {
        P2PManagerMaker() : P2PManager() {}
    };
    auto manager = std::make_shared<P2PManagerMaker>();
    manager->m_last_data_time = std::chrono::steady_clock::now();
    manager->init();
    return manager;
}

P2PManager::P2PManager()
    : m_io_context(),
      m_kcp_update_timer(m_io_context),
      m_cleanup_timer(m_io_context),
      m_worker_pool(std::thread::hardware_concurrency()) {
}

void P2PManager::set_state_manager(StateManager* sm) {
    m_state_manager = sm;
    if (m_transfer_manager) {
        m_transfer_manager->set_state_manager(sm);
    }
}

void P2PManager::set_tracker_client(TrackerClient* tc) { m_tracker_client = tc; }
void P2PManager::set_role(SyncRole role) { m_role = role; }

void P2PManager::set_stun_config(std::string host, uint16_t port) {
    m_stun_host = std::move(host);
    m_stun_port = port;
    g_logger->info("[Config] STUN 服务器设置为: {}:{}", m_stun_host, m_stun_port);
}

void P2PManager::set_turn_config(std::string host, uint16_t port, std::string username, std::string password) {
    m_turn_host = std::move(host);
    m_turn_port = port;
    m_turn_username = std::move(username);
    m_turn_password = std::move(password);
}

void P2PManager::init() {
    // 【重构】TransferManager 回调使用 PeerController
    // 【修复】减少锁持有时间，避免与 update_all_kcps 的锁竞争
    auto send_cb = [weak_self = weak_from_this()](const std::string& peer_id,
                                                  const std::string& encrypted_data) -> int {
        auto self = weak_self.lock();
        if (!self) return 0;

        // 先在锁内复制 controller，然后在锁外发送
        // 这样避免持有锁期间执行耗时的发送操作，减少锁竞争
        std::shared_ptr<PeerController> controller;
        {
            std::shared_lock<std::shared_mutex> lock(self->m_peers_mutex);  // 读操作
            auto it = self->m_peers.find(peer_id);
            if (it != self->m_peers.end() && it->second->is_connected()) {
                controller = it->second;
            }
        }
        
        // 锁外发送
        if (controller && controller->is_valid()) {
            return controller->send_message(encrypted_data);
        }
        return 0;
    };

    m_transfer_manager = std::make_shared<TransferManager>(m_state_manager, m_worker_pool, m_crypto, send_cb);
    m_thread = std::jthread([this]() {
        g_logger->info("[P2P] IO context 在后台线程运行...");
        auto work_guard = boost::asio::make_work_guard(m_io_context);
        m_io_context.run();
    });
    schedule_kcp_update();
    schedule_cleanup_task();
    init_upnp();
}

P2PManager::~P2PManager() {
    m_io_context.stop();
    m_worker_pool.join();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // 【重构】清理所有 PeerController
    std::unique_lock<std::shared_mutex> lock(m_peers_mutex);  // 写操作：清空
    for (auto& [peer_id, controller] : m_peers) {
        controller->close();
    }
    m_peers.clear();
}

// ═══════════════════════════════════════════════════════════════
// KCP 更新
// ═══════════════════════════════════════════════════════════════

void P2PManager::schedule_kcp_update() {
    m_kcp_update_timer.expires_after(std::chrono::milliseconds(m_kcp_update_interval_ms));
    m_kcp_update_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->update_all_kcps();
        }
    });
}

void P2PManager::update_all_kcps() {
    auto current_time_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    bool has_activity = false;
    
    // 【关键修复】先在锁内复制需要更新的 controller 列表
    // 然后在锁外调用 update_kcp()，避免死锁
    // 死锁场景：
    //   update_all_kcps() 持有 m_peers_mutex
    //   -> controller->update_kcp()
    //   -> m_kcp->receive() 触发回调
    //   -> handle_peer_message() 尝试获取 m_peers_mutex → 死锁！
    std::vector<std::shared_ptr<PeerController>> controllers_to_update;
    {
        std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
        for (auto& [peer_id, controller] : m_peers) {
            if (controller->is_connected()) {
                controllers_to_update.push_back(controller);
            }
        }
    }
    
    // 在锁外更新 KCP（回调可以安全地获取锁）
    for (auto& controller : controllers_to_update) {
        if (controller->is_valid()) {
            controller->update_kcp(current_time_ms);
            if (controller->get_kcp_wait_send() > 0) {
                has_activity = true;
            }
        }
    }

    // 自适应更新频率
    if (has_activity) {
        m_last_data_time = std::chrono::steady_clock::now();
        m_kcp_update_interval_ms = 5;
    } else {
        auto idle_duration = std::chrono::steady_clock::now() - m_last_data_time;
        if (idle_duration > std::chrono::seconds(5)) {
            m_kcp_update_interval_ms = 100;
        } else {
            m_kcp_update_interval_ms = 10;
        }
    }

    schedule_kcp_update();
}

// ═══════════════════════════════════════════════════════════════
// 【重构核心】连接管理 - 使用 PeerController
// ═══════════════════════════════════════════════════════════════

void P2PManager::connect_to_peers(const std::vector<std::string>& peer_addresses) {
    std::unique_lock<std::shared_mutex> lock(m_peers_mutex);  // 写操作：插入peer

    if (!m_tracker_client) {
        g_logger->error("[ICE] TrackerClient is null, 无法获取 self_id。");
        return;
    }
    std::string self_id = m_tracker_client->get_self_id();
    if (self_id.empty()) {
        g_logger->warn("[ICE] Self ID 尚未设置，推迟连接逻辑。");
        return;
    }

    for (const auto& peer_id : peer_addresses) {
        if (m_peers.count(peer_id)) {
            continue;
        }

        g_logger->info("[ICE] 正在为对等点 {} 创建 PeerController...", peer_id);

        // 【重构】创建 PeerController 回调
        PeerControllerCallbacks callbacks;
        
        // 状态变化回调
        callbacks.on_state_changed = [this, peer_id](PeerState state) {
            handle_peer_state_changed(peer_id, state);
        };
        
        // 信令回调 - 转发到 TrackerClient
        callbacks.on_signal_needed = [this, peer_id](const std::string& signal_type, const std::string& payload) {
            if (m_tracker_client) {
                m_tracker_client->send_signaling_message(peer_id, signal_type, payload);
            }
        };
        
        // 消息接收回调
        callbacks.on_message_received = [this, peer_id](const std::string& message) {
            handle_peer_message(peer_id, message);
        };

        // 创建 PeerController
        auto controller = PeerController::create(
            self_id,
            peer_id,
            m_io_context,
            create_ice_config(),
            std::move(callbacks)
        );

        if (!controller) {
            g_logger->error("[ICE] PeerController::create 失败 (对等点: {})", peer_id);
            continue;
        }

        m_peers[peer_id] = controller;

        // 根据角色决定是否主动发起连接
        if (controller->is_offer_side()) {
            g_logger->info("[ICE] 我们是 Offer 方，主动发起连接 (对于 {})", peer_id);
            controller->initiate_connection();
        } else {
            g_logger->info("[ICE] 我们是 Answer 方，等待 Offer (对于 {})", peer_id);
        }
    }
}

// 【重构】新增：处理 Peer 状态变化
void P2PManager::handle_peer_state_changed(const std::string& peer_id, PeerState state) {
    g_logger->info("[ICE] Peer {} 状态变化: {}", peer_id, static_cast<int>(state));
    
    if (state == PeerState::Connected) {
        g_logger->info("[ICE] ✅ 与 {} 建立连接成功！", peer_id);
        
        // 清除重连计数
        {
            std::lock_guard<std::mutex> lock(m_reconnect_mutex);
            m_reconnect_attempts.erase(peer_id);
            m_last_reconnect_time.erase(peer_id);
        }
        
        // 如果是 Source 或双向模式，触发可靠同步会话
        if (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional) {
            std::shared_ptr<PeerController> controller;
            {
                std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
                auto it = m_peers.find(peer_id);
                if (it != m_peers.end()) {
                    controller = it->second;
                }
            }
            
            if (controller) {
                g_logger->info("[P2P] 连接激活，准备向对等点 {} 推送文件状态...", peer_id);
                
                // 生成新的同步会话 ID
                uint64_t session_id = std::chrono::steady_clock::now().time_since_epoch().count();
                controller->sync_session_id.store(session_id);
                
                // 投递到 Worker 线程执行同步
                boost::asio::post(m_worker_pool, [this, self = shared_from_this(), controller, session_id]() {
                    perform_flood_sync(controller, session_id);
                });
            }
        }
    } else if (state == PeerState::Failed) {
        g_logger->warn("[ICE] ❌ 与 {} 连接失败，尝试重连...", peer_id);
        schedule_reconnect(peer_id);
    }
}

// 【重构】新增：处理 Peer 消息（来自 PeerController 的回调）
void P2PManager::handle_peer_message(const std::string& peer_id, const std::string& encrypted_msg) {
    // 查找对应的 PeerController
    std::shared_ptr<PeerController> controller;
    {
        std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
        auto it = m_peers.find(peer_id);
        if (it == m_peers.end()) {
            g_logger->warn("[KCP] 收到来自未知对等点 {} 的消息", peer_id);
            return;
        }
        controller = it->second;
    }
    
    // 处理消息
    handle_kcp_message(encrypted_msg, controller.get());
}

// ═══════════════════════════════════════════════════════════════
// 信令处理
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_signaling_message(const std::string& from_peer_id, 
                                          const std::string& message_type,
                                          const std::string& payload) {
    std::shared_ptr<PeerController> controller;
    {
        std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
        auto it = m_peers.find(from_peer_id);
        if (it == m_peers.end()) {
            g_logger->warn("[ICE] 收到来自未知对等点 {} 的信令消息。", from_peer_id);
            return;
        }
        controller = it->second;
    }
    
    if (message_type == "ice_candidate") {
        g_logger->debug("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    } else {
        g_logger->info("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    }
    
    // 【重构】转发到 PeerController 处理
    controller->handle_signaling(message_type, payload);
}

// ═══════════════════════════════════════════════════════════════
// Peer 断开/重连
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_peer_leave(const std::string& peer_id) {
    std::unique_lock<std::shared_mutex> lock(m_peers_mutex);  // 写操作：删除peer
    auto it = m_peers.find(peer_id);
    
    if (it == m_peers.end()) {
        // 可能已经通过 goodbye 处理过
        g_logger->debug("[P2P] peer_leave: {} 已不在连接池中", peer_id);
        return;
    }
    
    auto& controller = it->second;
    
    if (controller->is_graceful_shutdown.load()) {
        // 【情况 1】之前收到过 goodbye，是主动退出，已经处理过
        g_logger->debug("[P2P] peer_leave: {} 是主动退出（已收到 goodbye），跳过清理", peer_id);
    } else {
        // 【情况 2】没收到 goodbye，是网络断开/崩溃，保留传输状态等待续传
        g_logger->info("[P2P] {} 掉线（未收到 goodbye），保留传输状态等待续传", peer_id);
        // 注意：不调用 m_transfer_manager->cancel_receives_for_peer()
    }
    
    // 清理 PeerController（无论哪种情况都要做）
    controller->close();
    m_peers.erase(it);
}

void P2PManager::schedule_reconnect(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_reconnect_mutex);
    
    int& attempt_count = m_reconnect_attempts[peer_id];
    attempt_count++;
    
    if (attempt_count > MAX_RECONNECT_ATTEMPTS) {
        g_logger->error("[ICE] 对等点 {} 重连次数超过上限 ({})，放弃重连。", 
                        peer_id, MAX_RECONNECT_ATTEMPTS);
        m_reconnect_attempts.erase(peer_id);
        return;
    }
    
    int delay_ms = BASE_RECONNECT_DELAY_MS * (1 << (attempt_count - 1));
    g_logger->info("[ICE] 将在 {}ms 后尝试第 {} 次重连 (对等点: {})", 
                   delay_ms, attempt_count, peer_id);
    
    auto timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    timer->expires_after(std::chrono::milliseconds(delay_ms));
    timer->async_wait([this, self = shared_from_this(), peer_id, timer](const boost::system::error_code& ec) {
        if (ec) return;
        
        // 检查是否已经重新连接
        {
            std::unique_lock<std::shared_mutex> lock(m_peers_mutex);  // 写操作：删除peer
            auto it = m_peers.find(peer_id);
            if (it != m_peers.end() && it->second->is_connected()) {
                g_logger->info("[ICE] 对等点 {} 已重新连接，取消重连。", peer_id);
                return;
            }
            // 移除旧的 controller
            if (it != m_peers.end()) {
                it->second->close();
                m_peers.erase(it);
            }
        }
        
        // 重新连接
        connect_to_peers({peer_id});
    });
}

// ═══════════════════════════════════════════════════════════════
// 消息发送
// ═══════════════════════════════════════════════════════════════

void P2PManager::send_over_kcp(const std::string& msg) {
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    std::string encrypted_msg = m_crypto.encrypt(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，广播消息未发送");
        return;
    }
    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
    int sent_count = 0;
    for (auto& [peer_id, controller] : m_peers) {
        if (controller->is_connected()) {
            controller->send_message(encrypted_msg);
            sent_count++;
        }
    }
    if (sent_count > 0) {
        g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)", sent_count, encrypted_msg.length());
    }
}

void P2PManager::send_over_kcp_peer(const std::string& msg, PeerController* peer) {
    if (!peer || !peer->is_connected()) {
        g_logger->warn("[KCP] 尝试向无效或未就绪的对等点发送消息。");
        return;
    }
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    std::string encrypted_msg = m_crypto.encrypt(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，单播消息未发送至 {}", peer->get_peer_id());
        return;
    }
    peer->send_message(encrypted_msg);
}

void P2PManager::send_over_kcp_peer_safe(const std::string& msg, const std::string& peer_id) {
    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
    auto it = m_peers.find(peer_id);
    if (it != m_peers.end() && it->second->is_connected()) {
        send_over_kcp_peer(msg, it->second.get());
    }
}

// ═══════════════════════════════════════════════════════════════
// 消息处理 - 核心路由
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_kcp_message(const std::string& msg, PeerController* from_peer) {
    std::string decrypted_msg = m_crypto.decrypt(msg);
    if (decrypted_msg.empty()) {
        g_logger->warn("[KCP] 解密失败 ({} bytes 原始数据)", msg.size());
        return;
    }
    uint8_t msg_type = decrypted_msg[0];
    std::string payload(decrypted_msg.begin() + 1, decrypted_msg.end());

    bool can_receive = m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional;
    if (msg_type == MSG_TYPE_JSON) {
        try {
            auto json = nlohmann::json::parse(payload);
            const std::string json_msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
            auto& json_payload = json.at(Protocol::MSG_PAYLOAD);

            g_logger->info("[KCP] 收到 '{}' 消息 (来自: {})", json_msg_type,
                           from_peer ? from_peer->get_peer_id() : "<unknown>");

            if (json_msg_type == Protocol::TYPE_SHARE_STATE && can_receive) {
                handle_share_state(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE && can_receive) {
                handle_file_update(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_DELETE && can_receive) {
                handle_file_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_REQUEST_FILE) {
                bool can_serve = (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional);
                if (can_serve && from_peer) {
                    m_transfer_manager->queue_upload(from_peer->get_peer_id(), json_payload);
                }
            } else if (json_msg_type == Protocol::TYPE_DIR_CREATE && can_receive) {
                handle_dir_create(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_DIR_DELETE && can_receive) {
                handle_dir_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_SYNC_BEGIN && can_receive) {
                handle_sync_begin(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_SYNC_ACK) {
                handle_sync_ack(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_GOODBYE) {
                // 【断点续传】处理对端正常退出通知
                handle_goodbye(from_peer);
            } else {
                g_logger->warn("[KCP] 消息类型 '{}' 不适用于当前角色 ({})", json_msg_type,
                               m_role == SyncRole::Source ? "Source" : "Destination");
            }
        } catch (const std::exception& e) {
            g_logger->error("[P2P] 处理KCP JSON消息时发生错误: {}", e.what());
        }
    } else if (msg_type == MSG_TYPE_BINARY_CHUNK) {
        if (m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional) {
            std::string sender_id = from_peer ? from_peer->get_peer_id() : "";
            m_transfer_manager->handle_chunk(payload, sender_id);
        }
    } else {
        g_logger->error("[KCP] 收到未知消息类型: {}", (int)msg_type);
    }
}

TransferManager::SessionStats P2PManager::get_transfer_stats() {
    if (m_transfer_manager) {
        return m_transfer_manager->get_session_stats();
    }
    return {0, 0};
}

// ═══════════════════════════════════════════════════════════════
// 业务逻辑处理器 - handle_share_state
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_share_state(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role != SyncRole::Destination && m_mode != SyncMode::BiDirectional) return;

    std::string peer_id = from_peer ? from_peer->get_peer_id() : "";
    if (peer_id.empty()) return;

    int64_t safe_threshold_ts = from_peer ? (from_peer->connected_at_ts.load() - 5) : 0;

    g_logger->info("[KCP] (Destination) 收到来自 {} 的状态。连接TS: {}, 历史阈值: {}", peer_id,
                   from_peer->connected_at_ts.load(), safe_threshold_ts);

    boost::asio::post(m_worker_pool, [self = shared_from_this(), payload, peer_id, safe_threshold_ts]() {
        if (!self->m_state_manager) {
            g_logger->error("[Sync] StateManager 为空，无法处理状态。");
            return;
        }
        
        self->m_state_manager->scan_directory();
        
        std::vector<FileInfo> remote_files;
        std::set<std::string> remote_dirs;
        
        try {
            if (payload.contains("files")) {
                // 正确解析方式：files 是一个数组，每个元素包含 path/hash/mtime/size
                for (const auto& file_json : payload["files"]) {
                    FileInfo fi;
                    fi.path = file_json.value("path", "");
                    fi.modified_time = file_json.value("mtime", static_cast<uint64_t>(0));
                    fi.hash = file_json.value("hash", "");
                    fi.size = file_json.value("size", static_cast<uint64_t>(0));  // 【断点续传】添加 size
                    if (!fi.path.empty()) {
                        remote_files.push_back(fi);
                    }
                }
            }
            if (payload.contains("directories")) {
                for (const auto& dir : payload["directories"]) {
                    remote_dirs.insert(dir.get<std::string>());
                }
            }
        } catch(const std::exception& e) {
            g_logger->error("[Sync] 解析远程状态失败: {}", e.what());
            return;
        }
        
        // 使用 SyncManager 进行比较
        auto get_history = [self, peer_id](const std::string& path) -> std::optional<SyncHistory> {
            return self->m_state_manager->get_full_history(peer_id, path);
        };
        
        SyncActions file_actions = SyncManager::compare_states_and_get_requests(
            self->m_state_manager->get_all_files(), remote_files, get_history, self->m_mode);
        DirSyncActions dir_actions = SyncManager::compare_dir_states(
            self->m_state_manager->get_local_directories(), remote_dirs, self->m_mode);

        // E1. 删除多余文件
        if (!file_actions.files_to_delete.empty()) {
            g_logger->info("[Sync] 计划删除 {} 个本地多余的文件。", file_actions.files_to_delete.size());
            for (const auto& file_path_str : file_actions.files_to_delete) {
                std::filesystem::path relative_path = Utf8ToPath(file_path_str);
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;
                std::error_code ec;

                if (std::filesystem::remove(full_path, ec)) {
                    g_logger->info("[Sync] -> 已删除 (相对路径): {}", file_path_str);
                    self->m_state_manager->remove_path_from_map(file_path_str);
                } else if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", file_path_str, FormatErrorCode(ec));
                }
            }
        }

        // E2. 删除多余目录
        if (!dir_actions.dirs_to_delete.empty()) {
            std::vector<std::string> sorted_dirs = dir_actions.dirs_to_delete;
            std::sort(sorted_dirs.begin(), sorted_dirs.end(),
                      [](const std::string& a, const std::string& b) { return a.length() > b.length(); });

            for (const auto& dir_path_str : sorted_dirs) {
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                bool deleted = false;

                if (self->m_mode == SyncMode::OneWay) {
                    if (std::filesystem::remove_all(full_path, ec) != static_cast<std::uintmax_t>(-1)) {
                        deleted = true;
                    }
                } else {
                    if (std::filesystem::remove(full_path, ec)) {
                        deleted = true;
                    } else if (ec && ec != std::errc::directory_not_empty) {
                        g_logger->warn("[Sync] 删除目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                    }
                }

                if (deleted || (!deleted && !std::filesystem::exists(full_path))) {
                    self->m_state_manager->remove_dir_from_map(dir_path_str);
                }
            }
        }

        // E3. 创建缺失目录
        if (!dir_actions.dirs_to_create.empty()) {
            for (const auto& dir_path_str : dir_actions.dirs_to_create) {
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                std::filesystem::create_directories(full_path, ec);
                if (!ec) {
                    self->m_state_manager->add_dir_to_map(dir_path_str);
                } else {
                    g_logger->warn("[Sync] 创建目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                }
            }
        }

        // F. 发送文件请求
        if (!file_actions.files_to_request.empty()) {
            g_logger->info("[KCP] 计划向 {} 请求 {} 个缺失/过期的文件。", peer_id,
                           file_actions.files_to_request.size());
            
            // 构建文件路径到 FileInfo 的映射，用于获取 hash/size
            std::map<std::string, FileInfo> remote_file_map;
            for (const auto& fi : remote_files) {
                remote_file_map[fi.path] = fi;
            }

            boost::asio::post(self->m_io_context, [self, peer_id, 
                              reqs = std::move(file_actions.files_to_request),
                              remote_file_map = std::move(remote_file_map)]() {
                std::shared_lock<std::shared_mutex> lock(self->m_peers_mutex);  // 读操作
                auto it = self->m_peers.find(peer_id);
                if (it == self->m_peers.end() || !it->second->is_connected()) return;

                auto* peer_ctrl = it->second.get();
                for (const auto& file_path : reqs) {
                    nlohmann::json request_msg;
                    request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                    
                    // 【断点续传】获取远程文件信息
                    std::string remote_hash;
                    uint64_t remote_size = 0;
                    auto fit = remote_file_map.find(file_path);
                    if (fit != remote_file_map.end()) {
                        remote_hash = fit->second.hash;
                        remote_size = fit->second.size;
                    }
                    
                    // 【断点续传】检查是否可以续传
                    auto resume_info = self->m_transfer_manager->check_resume_eligibility(
                        file_path, remote_hash, remote_size);
                    
                    if (resume_info) {
                        // 可以续传
                        request_msg[Protocol::MSG_PAYLOAD] = {
                            {"path", file_path},
                            {"start_chunk", resume_info->received_chunks},
                            {"expected_hash", resume_info->expected_hash},
                            {"expected_size", resume_info->expected_size}
                        };
                        g_logger->info("[P2P] 发送续传请求: {} 从 chunk #{} 开始", 
                                      file_path, resume_info->received_chunks);
                    } else {
                        // 新传输，预注册元数据
                        self->m_transfer_manager->register_expected_metadata(
                            file_path, peer_id, remote_hash, remote_size);
                        request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
                    }
                    
                    self->send_over_kcp_peer(request_msg.dump(), peer_ctrl);
                }
            });
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 业务逻辑处理器 - handle_file_update
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_file_update(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;

    if (from_peer) {
        from_peer->received_file_count.fetch_add(1);
        
        std::string pid = from_peer->get_peer_id();
        uint64_t sid = from_peer->sync_session_id.load();
        
        boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
             std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
             auto it = m_peers.find(pid);
             if (it != m_peers.end() && it->second->sync_session_id.load() == sid) {
                 if (it->second->sync_timeout_timer) {
                     it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
                 }
             }
        });
    }
    
    if (!m_state_manager) return;
    
    FileInfo remote_info;
    try {
        remote_info = payload.get<FileInfo>();
    } catch (const std::exception& e) {
        g_logger->error("[KCP] 解析 file_update 失败: {}", e.what());
        return;
    }

    std::string peer_id = from_peer ? from_peer->get_peer_id() : "";
    if (peer_id.empty()) return;

    // Offload 耗时操作到 Worker 线程
    boost::asio::post(m_worker_pool, [this, self = shared_from_this(), remote_info, peer_id]() {
        // --- 1. 拦截回声 (Echo Check) ---
        if (m_state_manager->should_ignore_echo(peer_id, remote_info.path, remote_info.hash)) {
            return;
        }

        g_logger->info("[P2P] 收到更新请求: {}", remote_info.path);

        std::filesystem::path relative_path = Utf8ToPath(remote_info.path);
        std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;

        bool should_request = false;
        std::error_code ec;

        // --- 2. 冲突检测 (Conflict Resolution) ---
        if (!std::filesystem::exists(full_path, ec)) {
            // 情况 0: 本地没有该文件 -> 直接请求 (Create/New)
            g_logger->info("[Sync] 本地缺失，准备下载: {}", remote_info.path);
            should_request = true;
        } else {
            std::string remote_hash = remote_info.hash;
            // 【耗时操作】在 Worker 线程计算 Hash
            std::string local_hash = Hashing::CalculateSHA256(full_path);
            std::string base_hash = m_state_manager->get_base_hash(peer_id, remote_info.path);

            if (local_hash == remote_hash) {
                g_logger->info("[Sync] 内容一致，无需更新: {}", remote_info.path);
                m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                return;
            }

            if (base_hash.empty() || local_hash == base_hash) {
                g_logger->info("[Sync] 正常更新 (Local==Base): {}", remote_info.path);
                should_request = true;
            } else {
                g_logger->warn("[Sync] ⚠️ 检测到冲突: {}", remote_info.path);
                g_logger->warn("       Base: {}...", base_hash.substr(0, std::min<size_t>(6, base_hash.size())));
                g_logger->warn("       Local: {}...", local_hash.substr(0, std::min<size_t>(6, local_hash.size())));
                g_logger->warn("       Remote: {}...", remote_hash.substr(0, std::min<size_t>(6, remote_hash.size())));

                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

                std::string filename = relative_path.stem().string();
                std::string ext = relative_path.extension().string();
                std::string conflict_name = filename + ".conflict." + std::to_string(timestamp) + ext;
                std::filesystem::path conflict_path = full_path.parent_path() / conflict_name;

                std::error_code ren_ec;
                std::filesystem::rename(full_path, conflict_path, ren_ec);

                if (!ren_ec) {
                    g_logger->warn("[Sync] ⚡ 本地冲突文件已重命名为: {}", conflict_path.filename().string());
                    should_request = true;
                } else {
                    g_logger->error("[Sync] ❌ 冲突处理失败 (无法重命名): {} | {}", remote_info.path, FormatErrorCode(ren_ec));
                    return;
                }
            }
        }

        if (should_request) {
            nlohmann::json request_msg;
            request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
            
            // 【断点续传】检查是否可以续传
            auto resume_info = m_transfer_manager->check_resume_eligibility(
                remote_info.path, remote_info.hash, remote_info.size);
            
            if (resume_info) {
                // 可以续传
                request_msg[Protocol::MSG_PAYLOAD] = {
                    {"path", remote_info.path},
                    {"start_chunk", resume_info->received_chunks},
                    {"expected_hash", resume_info->expected_hash},
                    {"expected_size", resume_info->expected_size}
                };
                g_logger->info("[P2P] 发送续传请求: {} 从 chunk #{} 开始", 
                              remote_info.path, resume_info->received_chunks);
            } else {
                // 新传输，预注册元数据
                m_transfer_manager->register_expected_metadata(
                    remote_info.path, peer_id, remote_info.hash, remote_info.size);
                request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
            }
            
            // 【注意】必须发回到 IO 线程发送 KCP確保线程安全
            std::string msg_str = request_msg.dump();
            boost::asio::post(m_io_context, [self, peer_id, msg_str]() {
                 self->send_over_kcp_peer_safe(msg_str, peer_id);
            });
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 业务逻辑处理器 - handle_file_delete
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_file_delete(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_file_count.fetch_add(1);
        
        std::string pid = from_peer->get_peer_id();
        uint64_t sid = from_peer->sync_session_id.load();
        boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
             std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
             auto it = m_peers.find(pid);
             if (it != m_peers.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                 it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
             }
        });
    }

    if (!m_state_manager) return;
    
    // 直接扣到 Worker 线程，不阻塞网络
    boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 file_delete 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量删除: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        if (std::filesystem::remove(full_path, ec)) {
            g_logger->info("[Sync] -> 已删除本地文件: {}", relative_path_str);
            m_state_manager->remove_path_from_map(relative_path_str);
        } else {
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            } else {
                g_logger->debug("[Sync] 文件已不存在: {}", relative_path_str);
            }
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 业务逻辑处理器 - handle_dir_create / handle_dir_delete
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_dir_create(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_dir_count.fetch_add(1);
        
        // 刷新超时定时器
        std::string pid = from_peer->get_peer_id();
        uint64_t sid = from_peer->sync_session_id.load();
        boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
             std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
             auto it = m_peers.find(pid);
             if (it != m_peers.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                 it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
             }
        });
    }
    
    if (!m_state_manager) return;
    
    // Worker 线程处理
    boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 dir_create 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录创建: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        if (std::filesystem::create_directories(full_path, ec)) {
            g_logger->info("[Sync] -> 已创建目录: {}", relative_path_str);
            m_state_manager->add_dir_to_map(relative_path_str);
        } else if (ec) {
            g_logger->error("[Sync] ❌ 创建目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
        }
    });
}

void P2PManager::handle_file_request(const nlohmann::json& payload, PeerController* from_peer) {
    // 文件请求由 TransferManager 处理，这里只是占位
}

void P2PManager::handle_dir_delete(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_dir_count.fetch_add(1);

        // 刷新超时定时器
        std::string pid = from_peer->get_peer_id();
        uint64_t sid = from_peer->sync_session_id.load();
        boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
             std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
             auto it = m_peers.find(pid);
             if (it != m_peers.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                 it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
             }
        });
    }
    
    if (!m_state_manager) return;
    
    // Worker 线程处理
    boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (...) {
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录删除: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        std::filesystem::remove_all(full_path, ec);

        if (!ec) {
            g_logger->info("[Sync] -> 已删除目录: {}", relative_path_str);
            m_state_manager->remove_dir_from_map(relative_path_str);
        } else {
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] ❌ 删除目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            } else {
                m_state_manager->remove_dir_from_map(relative_path_str);
            }
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 文件传输相关
// ═══════════════════════════════════════════════════════════════

void P2PManager::schedule_cleanup_task() {
    m_cleanup_timer.expires_after(std::chrono::minutes(5));
    m_cleanup_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->cleanup_stale_buffers();
            self->schedule_cleanup_task();
        }
    });
}

std::vector<TransferStatus> P2PManager::get_active_transfers() {
    if (m_transfer_manager) {
        return m_transfer_manager->get_active_transfers();
    }
    return {};
}

void P2PManager::cleanup_stale_buffers() {
    if (m_transfer_manager) {
        m_transfer_manager->cleanup_stale_buffers();
    }
}

// ═══════════════════════════════════════════════════════════════
// UPnP
// ═══════════════════════════════════════════════════════════════

void P2PManager::init_upnp() {
    // 在 Worker 线程池中执行，避免阻塞
    // 使用 weak_ptr 确保生命周期安全
    std::weak_ptr<P2PManager> weak_self = weak_from_this();
    
    boost::asio::post(m_worker_pool, [weak_self]() {
        auto self = weak_self.lock();
        if (!self) return;  // 对象已销毁
        
        int error = 0;
        // 发现路由器 (2000ms 超时)
        struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        
        // 再次检查对象是否仍有效
        self = weak_self.lock();
        if (!self) {
            if (devlist) freeUPNPDevlist(devlist);
            return;
        }
        
        std::lock_guard<std::mutex> lock(self->m_upnp_mutex);
        if (devlist) {
            g_logger->info("[UPnP] 发现 UPnP 设备列表。");
            
            // 获取有效的 IGD (互联网网关设备)
            char wanaddr[64] = {0};
            int r = UPNP_GetValidIGD(devlist, &self->m_upnp_urls, &self->m_upnp_data, 
                                     self->m_upnp_lan_addr, sizeof(self->m_upnp_lan_addr),
                                     wanaddr, sizeof(wanaddr));

            if (r == 1) {
                g_logger->info("[UPnP] 成功连接到路由器: {}", self->m_upnp_urls.controlURL);
                g_logger->info("[UPnP] 我们的局域网 IP: {}", self->m_upnp_lan_addr);

                // 获取公网 IP
                char public_ip[40];
                r = UPNP_GetExternalIPAddress(self->m_upnp_urls.controlURL, 
                                              self->m_upnp_data.first.servicetype, public_ip);

                if (r == UPNPCOMMAND_SUCCESS) {
                    self->m_upnp_public_ip = public_ip;
                    self->m_upnp_available = true;
                    g_logger->info("[UPnP] ✅ 成功获取公网 IP: {}", self->m_upnp_public_ip);
                } else {
                    g_logger->warn("[UPnP] 无法获取公网 IP (错误码: {}).", r);
                }
            } else {
                g_logger->warn("[UPnP] 未找到有效的 IGD (互联网网关设备).");
            }
            freeUPNPDevlist(devlist);
        } else {
            g_logger->warn("[UPnP] 未发现 UPnP 设备 (错误: {}).", error);
        }
    });
}

std::string get_sdp_field(const std::string& sdp, int index) {
    std::istringstream ss(sdp);
    std::string token;
    for (int i = 0; i <= index && std::getline(ss, token, ' '); ++i) {
        if (i == index) return token;
    }
    return "";
}

std::string P2PManager::rewrite_candidate(const std::string& sdp_candidate) {
    std::lock_guard<std::mutex> lock(m_upnp_mutex);

    // 如果 UPnP 不可用，或者我们没有公网IP，则不重写
    if (!m_upnp_available || m_upnp_public_ip.empty()) {
        return sdp_candidate;
    }

    // libjuice 的候选地址格式: "a=candidate:..."
    // 我们只关心 "host" 类型的候选地址，它们包含局域网IP
    std::string cand_type = get_sdp_field(sdp_candidate, 7);
    if (cand_type != "host") {
        return sdp_candidate;  // 不是 "host"，可能是 "srflx" 或 "relay"，直接返回
    }

    // "a=candidate:..." 字段: 4=ip, 5=port
    std::string local_ip = get_sdp_field(sdp_candidate, 4);
    std::string local_port = get_sdp_field(sdp_candidate, 5);

    // 确保是我们自己的局域网 IP
    if (local_ip != m_upnp_lan_addr) {
        g_logger->debug("[UPnP] 候选 IP {} 与 UPnP 局域网 IP {} 不匹配，跳过。", local_ip, m_upnp_lan_addr);
        return sdp_candidate;
    }

    // 尝试在路由器上添加这个端口映射
    // (将 公网端口 映射到 局域网IP:局域网端口)
    int r = UPNP_AddPortMapping(m_upnp_urls.controlURL, m_upnp_data.first.servicetype,
                                local_port.c_str(),  // external_port (使用与内部相同的端口)
                                local_port.c_str(),  // internal_port
                                m_upnp_lan_addr,     // internal_client
                                "VeritasSync P2P",   // description
                                "UDP",               // protocol
                                nullptr, "0");       // remote_host, duration

    if (r == UPNPCOMMAND_SUCCESS) {
        g_logger->info("[UPnP] 成功为候选地址 {}:{} 映射公网端口 {}", local_ip, local_port, local_port);

        // 成功！现在重写候选地址，用公网IP替换局域网IP
        std::string rewritten_candidate = sdp_candidate;
        size_t pos = rewritten_candidate.find(local_ip);
        if (pos != std::string::npos) {
            rewritten_candidate.replace(pos, local_ip.length(), m_upnp_public_ip);
            g_logger->info("[UPnP] 重写候选地址为: {}...", rewritten_candidate.substr(0, 40));
            return rewritten_candidate;
        }
    } else {
        g_logger->warn("[UPnP] 无法为 {}:{} 映射端口 (错误码: {}).", local_ip, local_port, r);
    }

    // 映射失败，返回原始候选地址
    return sdp_candidate;
}

// ═══════════════════════════════════════════════════════════════
// 同步会话管理
// ═══════════════════════════════════════════════════════════════

void P2PManager::send_sync_begin(PeerController* peer, uint64_t session_id, size_t file_count, size_t dir_count) {
    if (!peer) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_BEGIN;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"file_count", file_count},
        {"dir_count", dir_count}
    };
    send_over_kcp_peer(msg.dump(), peer);
}

void P2PManager::send_sync_ack(PeerController* peer, uint64_t session_id, size_t received_files, size_t received_dirs) {
    if (!peer) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"received_files", received_files},
        {"received_dirs", received_dirs}
    };
    send_over_kcp_peer(msg.dump(), peer);
}

void P2PManager::handle_sync_begin(const nlohmann::json& payload, PeerController* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t session_id = payload.at("session_id").get<uint64_t>();
        size_t file_count = payload.at("file_count").get<size_t>();
        size_t dir_count = payload.at("dir_count").get<size_t>();
        
        g_logger->info("[Sync] 收到同步开始: session={}, files={}, dirs={}", 
                       session_id, file_count, dir_count);
        
        from_peer->sync_session_id.store(session_id);
        from_peer->expected_file_count.store(file_count);
        from_peer->expected_dir_count.store(dir_count);
        from_peer->received_file_count.store(0);
        from_peer->received_dir_count.store(0);
        
        // 设置超时定时器
        std::string peer_id = from_peer->get_peer_id();
        from_peer->sync_timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
        from_peer->sync_timeout_timer->expires_after(std::chrono::seconds(60));
        from_peer->sync_timeout_timer->async_wait(
            [this, self = shared_from_this(), peer_id, session_id](const boost::system::error_code& ec) {
                if (ec) return;  // 被取消
                
                std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
                auto it = m_peers.find(peer_id);
                if (it == m_peers.end()) return;
                
                auto& peer = it->second;
                if (peer->sync_session_id.load() != session_id) return;
                
                size_t recv_files = peer->received_file_count.load();
                size_t expect_files = peer->expected_file_count.load();
                size_t recv_dirs = peer->received_dir_count.load();
                size_t expect_dirs = peer->expected_dir_count.load();
                
                if (recv_files < expect_files || recv_dirs < expect_dirs) {
                    g_logger->warn("[Sync] 同步会话 {} 超时 (文件: {}/{}, 目录: {}/{})", 
                                   session_id, recv_files, expect_files, recv_dirs, expect_dirs);
                }
                
                send_sync_ack(peer.get(), session_id, recv_files, recv_dirs);
                peer->sync_session_id.store(0);
            });
            
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 处理 sync_begin 失败: {}", e.what());
    }
}

void P2PManager::handle_sync_ack(const nlohmann::json& payload, PeerController* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t ack_session_id = payload.at("session_id").get<uint64_t>();
        size_t received_files = payload.at("received_files").get<size_t>();
        size_t received_dirs = payload.at("received_dirs").get<size_t>();
        
        // 【关键】检查这是否针对当前活跃会话的回复
        uint64_t current_id = from_peer->sync_session_id.load();
        if (ack_session_id != current_id) {
            g_logger->warn("[Sync] 收到过时或不匹配的 ACK (ACK ID: {}, 当前 ID: {})，忽略。", ack_session_id, current_id);
            return;
        }

        g_logger->warn("[Sync] 收到 ACK (ID: {}): 对方只收到 {} 文件, {} 目录。重新同步...",
                       ack_session_id, received_files, received_dirs);
        
        // 既然对方没收完，且会话 ID 匹配，说明确实需要补发
        // 生成一个新的 ID 并重新开始推送
        std::string peer_id = from_peer->get_peer_id();
        uint64_t new_session_id = std::chrono::steady_clock::now().time_since_epoch().count();
        from_peer->sync_session_id.store(new_session_id);
        
        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), peer_id, new_session_id]() {
            // 重新获取 controller 的 shared_ptr
            std::shared_ptr<PeerController> controller;
            {
                std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
                auto it = m_peers.find(peer_id);
                if (it != m_peers.end()) {
                    controller = it->second;
                }
            }
            if (controller) {
                perform_flood_sync(controller, new_session_id);
            }
        });
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 解析 sync_ack 失败: {}", e.what());
    }
}

void P2PManager::perform_flood_sync(std::shared_ptr<PeerController> controller, uint64_t session_id) {
    if (!controller || !controller->is_valid() || !m_state_manager) {
        g_logger->warn("[Sync] perform_flood_sync: 上下文无效，跳过");
        return;
    }
    
    std::string peer_id = controller->get_peer_id();
    
    // 检查 session_id 是否一致（防止重复执行旧会话）
    if (controller->sync_session_id.load() != session_id) {
        g_logger->info("[Sync] 会话 ID 已变更，跳过本次同步");
        return;
    }
    
    // 1. 扫描目录获取所有文件
    m_state_manager->scan_directory();
    std::vector<FileInfo> files = m_state_manager->get_all_files();
    std::set<std::string> dirs = m_state_manager->get_local_directories();

    if (files.empty() && dirs.empty()) {
        g_logger->info("[P2P] 没有文件需要推送给 {}", peer_id);
        return;
    }

    g_logger->info("[P2P] 开始向 {} 推送 {} 个文件和 {} 个目录 (session: {})...", 
                   peer_id, files.size(), dirs.size(), session_id);

    // 2. 【关键】先发送 sync_begin 通知对方预期数量
    boost::asio::post(m_io_context, [this, self = shared_from_this(), controller, session_id, 
                                     file_count = files.size(), dir_count = dirs.size()]() {
        if (controller->is_valid()) {
            send_sync_begin(controller.get(), session_id, file_count, dir_count);
        }
    });

    // 3. 发送目录信息
    for (const auto& dir_path : dirs) {
        if (!controller->is_valid()) {
            g_logger->warn("[P2P] 连接已断开，停止发送目录 (session: {})", session_id);
            return;
        }
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_CREATE;
        msg[Protocol::MSG_PAYLOAD] = {{"path", dir_path}};

        std::weak_ptr<PeerController> weak_ctrl = controller;
        boost::asio::post(m_io_context, [self = shared_from_this(), weak_ctrl, msg_str = msg.dump()]() {
            auto ctrl_locked = weak_ctrl.lock();
            if (ctrl_locked && ctrl_locked->is_valid() && ctrl_locked->is_connected()) {
                self->send_over_kcp_peer(msg_str, ctrl_locked.get());
            }
        });
    }

    // 4. 逐个发送文件状态
    for (size_t i = 0; i < files.size(); ++i) {
        // 【关键】检查连接有效性和会话有效性
        if (!controller->is_valid()) {
            g_logger->warn("[P2P] 连接已断开，停止发送文件 (session: {}, 已发送 {}/{})", 
                           session_id, i, files.size());
            return;
        }
        
        if (controller->sync_session_id.load() != session_id) {
            g_logger->info("[Sync] 会话 ID 已变更，停止本次同步 (已发送 {}/{})", i, files.size());
            return;
        }
        
        const auto& file = files[i];
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
        msg[Protocol::MSG_PAYLOAD] = file;

        std::weak_ptr<PeerController> weak_ctrl = controller;
        boost::asio::post(m_io_context, [self = shared_from_this(), weak_ctrl, msg_str = msg.dump()]() {
            auto ctrl_locked = weak_ctrl.lock();
            if (ctrl_locked && ctrl_locked->is_valid() && ctrl_locked->is_connected()) {
                self->send_over_kcp_peer(msg_str, ctrl_locked.get());
            }
        });

        // 【流控】每 50 个文件检查一次 KCP 发送队列积压量
        if ((i + 1) % 50 == 0) {
            int pending = controller->get_kcp_wait_send();

            while (pending > 1024 && controller->is_valid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                pending = controller->get_kcp_wait_send();
            }
        }
    }

    g_logger->info("[P2P] 向 {} 推送文件状态完成 ({} 个文件, {} 个目录, session: {})", 
                   peer_id, files.size(), dirs.size(), session_id);
}

// ═══════════════════════════════════════════════════════════════
// 断点续传相关方法
// ═══════════════════════════════════════════════════════════════

void P2PManager::shutdown_gracefully() {
    g_logger->info("[P2P] 正在进行优雅关闭...");
    
    // 1. 广播 goodbye 给所有对端
    broadcast_goodbye();
    
    // 2. 等待发送完成
    wait_for_kcp_flush(500);  // 最多等待 500ms
    
    g_logger->info("[P2P] 优雅关闭完成");
}

void P2PManager::broadcast_goodbye() {
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_GOODBYE;
    msg[Protocol::MSG_PAYLOAD] = nlohmann::json::object();
    
    std::string msg_str = msg.dump();
    std::string encrypted_msg = m_crypto.encrypt(msg_str);
    
    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
    int sent_count = 0;
    
    for (auto& [peer_id, controller] : m_peers) {
        if (controller->is_connected()) {
            controller->send_message(encrypted_msg);
            controller->flush_kcp();  // 强制刷新
            sent_count++;
            g_logger->info("[P2P] 向 {} 发送 goodbye", peer_id);
        }
    }
    
    g_logger->info("[P2P] goodbye 已发送给 {} 个对等点", sent_count);
}

void P2PManager::wait_for_kcp_flush(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() 
                    + std::chrono::milliseconds(timeout_ms);
    
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_flushed = true;
        
        {
            std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
            for (auto& [peer_id, controller] : m_peers) {
                if (controller->get_kcp_wait_send() > 0) {
                    all_flushed = false;
                    break;
                }
            }
        }
        
        if (all_flushed) {
            g_logger->debug("[P2P] 所有 KCP 发送队列已清空");
            return;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    g_logger->warn("[P2P] KCP flush 超时，部分消息可能未送达");
}

void P2PManager::handle_goodbye(PeerController* from_peer) {
    if (!from_peer) return;
    
    std::string peer_id = from_peer->get_peer_id();
    g_logger->info("[P2P] 收到来自 {} 的 goodbye（程序正常关闭）", peer_id);
    
    // 标记为主动退出
    from_peer->is_graceful_shutdown.store(true);
    
    // 清理该 peer 的所有传输状态
    if (m_transfer_manager) {
        m_transfer_manager->cancel_receives_for_peer(peer_id);
    }
    
    // 清理 PeerController
    {
        std::unique_lock<std::shared_mutex> lock(m_peers_mutex);
        auto it = m_peers.find(peer_id);
        if (it != m_peers.end()) {
            it->second->close();
            m_peers.erase(it);
        }
    }
}

}  // namespace VeritasSync
