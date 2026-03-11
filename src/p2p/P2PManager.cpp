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
#include <thread>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/p2p/TrackerClient.h"
#define BUFFERSIZE 32768
#include <b64/decode.h>
#include <b64/encode.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

bool can_broadcast(SyncRole role, SyncMode mode) {
    if (role == SyncRole::Source) return true;
    if (mode == SyncMode::BiDirectional) return true;
    return false;
}

// ═══════════════════════════════════════════════════════════════
// 新增辅助函数
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

        // 加密已下沉到 PeerController
        std::string& final_msg = json_packet;

        // 使用 m_peers 替代 m_peers_by_agent
        boost::asio::post(self->m_io_context, [self, final_msg]() {
            std::shared_lock<std::shared_mutex> lock(self->m_peers_mutex);  // 读操作
            int sent_count = 0;
            for (auto& [peer_id, controller] : self->m_peers) {
                if (controller->is_connected()) {
                    controller->send_message(final_msg);
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
// 批量广播方法 (阶段1优化)
// ═══════════════════════════════════════════════════════════════

// 批量大小配置
static constexpr size_t FILE_UPDATE_BATCH_SIZE = 50;  // 每批最多 50 个文件更新
static constexpr size_t FILE_DELETE_BATCH_SIZE = 100; // 每批最多 100 个文件删除

void P2PManager::broadcast_file_updates_batch(const std::vector<FileInfo>& files) {
    if (!can_broadcast(m_role, m_mode)) return;
    if (files.empty()) return;
    
    g_logger->info("[P2P] (Source) 批量广播 {} 个文件更新", files.size());
    
    // 分批发送
    for (size_t i = 0; i < files.size(); i += FILE_UPDATE_BATCH_SIZE) {
        size_t end = std::min(i + FILE_UPDATE_BATCH_SIZE, files.size());
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE_BATCH;
        msg[Protocol::MSG_PAYLOAD]["files"] = nlohmann::json::array();
        
        for (size_t j = i; j < end; ++j) {
            msg[Protocol::MSG_PAYLOAD]["files"].push_back(files[j]);
        }
        
        send_over_kcp(msg.dump());
        
        g_logger->debug("[P2P] 发送文件更新批次 {}/{} ({} 个文件)", 
                       (i / FILE_UPDATE_BATCH_SIZE) + 1,
                       (files.size() + FILE_UPDATE_BATCH_SIZE - 1) / FILE_UPDATE_BATCH_SIZE,
                       end - i);
    }
}

void P2PManager::broadcast_file_deletes_batch(const std::vector<std::string>& paths) {
    if (!can_broadcast(m_role, m_mode)) return;
    if (paths.empty()) return;
    
    g_logger->info("[P2P] (Source) 批量广播 {} 个文件删除", paths.size());
    
    // 分批发送
    for (size_t i = 0; i < paths.size(); i += FILE_DELETE_BATCH_SIZE) {
        size_t end = std::min(i + FILE_DELETE_BATCH_SIZE, paths.size());
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE_BATCH;
        msg[Protocol::MSG_PAYLOAD]["paths"] = nlohmann::json::array();
        
        for (size_t j = i; j < end; ++j) {
            msg[Protocol::MSG_PAYLOAD]["paths"].push_back(paths[j]);
        }
        
        send_over_kcp(msg.dump());
    }
}

void P2PManager::broadcast_dir_changes_batch(const std::vector<std::string>& creates, 
                                              const std::vector<std::string>& deletes) {
    if (!can_broadcast(m_role, m_mode)) return;
    if (creates.empty() && deletes.empty()) return;
    
    g_logger->info("[P2P] (Source) 批量广播目录变更: {} 创建, {} 删除", 
                   creates.size(), deletes.size());
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_BATCH;
    msg[Protocol::MSG_PAYLOAD]["creates"] = creates;
    msg[Protocol::MSG_PAYLOAD]["deletes"] = deletes;
    
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
    if (m_sync_handler) {
        m_sync_handler->set_state_manager(sm);
    }
    if (m_sync_session) {
        m_sync_session->set_state_manager(sm);
    }
}

void P2PManager::set_tracker_client(TrackerClient* tc) { m_tracker_client = tc; }
void P2PManager::set_role(SyncRole role) {
    m_role = role;
    if (m_sync_handler) m_sync_handler->set_role(role);
    if (m_sync_session) m_sync_session->set_role(role);
}

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
    // TransferManager 回调使用 PeerController
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
            // TransferManager 发来的是明文
            return controller->send_message(encrypted_data); 
        }
        return -1;  // 【断点续传】连接已断开，返回 -1 通知发送方提前终止
    };
    // 创建 TransferManager 传输管理器
    m_transfer_manager = std::make_shared<TransferManager>(m_state_manager, m_io_context, m_worker_pool, send_cb);
    
    // 创建 SyncHandler 同步消息处理器
    m_sync_handler = std::make_unique<SyncHandler>(
        m_state_manager,
        m_transfer_manager,
        m_worker_pool,
        m_io_context,
        // send_to_peer 回调
        [this](const std::string& msg, PeerController* peer) {
            send_over_kcp_peer(msg, peer);
        },
        // send_to_peer_safe 回调
        [this](const std::string& msg, const std::string& peer_id) {
            send_over_kcp_peer_safe(msg, peer_id);
        },
        // with_peer 回调：在 peers 锁内查找 peer 并执行操作
        [this](const std::string& peer_id, std::function<void(PeerController*)> action) {
            std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
            auto it = m_peers.find(peer_id);
            if (it != m_peers.end() && it->second->is_connected()) {
                action(it->second.get());
            }
        }
    );
    m_sync_handler->set_role(m_role);
    m_sync_handler->set_mode(m_mode);
    
    // 创建 SyncSession 同步会话管理器
    m_sync_session = std::make_unique<SyncSession>(
        m_state_manager,
        m_worker_pool,
        m_io_context,
        // send_to_peer 回调
        [this](const std::string& msg, PeerController* peer) {
            send_over_kcp_peer(msg, peer);
        },
        // with_peer 回调：在 peers 锁内查找 peer 并执行操作
        [this](const std::string& peer_id, std::function<void(PeerController*)> action) {
            std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
            auto it = m_peers.find(peer_id);
            if (it != m_peers.end() && it->second->is_connected()) {
                action(it->second.get());
            }
        },
        // get_peer 回调：获取 PeerController shared_ptr
        [this](const std::string& peer_id) -> std::shared_ptr<PeerController> {
            std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
            auto it = m_peers.find(peer_id);
            if (it != m_peers.end()) {
                return it->second;
            }
            return nullptr;
        }
    );
    m_sync_session->set_role(m_role);
    m_sync_session->set_mode(m_mode);
    
    // 启动io线程
    m_thread = std::jthread([this]() {
        g_logger->info("[P2P] IO context 在后台线程运行...");
        auto work_guard = boost::asio::make_work_guard(m_io_context);
        m_io_context.run();
    });
    // 启动定时器
    schedule_kcp_update();
    schedule_cleanup_task();
    schedule_heartbeat();  // 启动心跳保活定时器
    // upnp发现（已迁移到 UpnpManager）
    m_upnp.init_async(m_worker_pool);
}

P2PManager::~P2PManager() {
    m_io_context.stop();    // 停止事件循环
    m_worker_pool.join();   // 等待所有工作线程完成
    if (m_thread.joinable()) {
        m_thread.join();    // 等待IO线程退出
    }
    // 清理所有 PeerController
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
// 连接管理 - 使用 PeerController
// ═══════════════════════════════════════════════════════════════

void P2PManager::connect_to_peers(const std::vector<std::string>& peer_addresses, bool force) {
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
        // 【修复】跳过自己的 ID，防止尝试连接自己
        if (peer_id == self_id) {
            g_logger->debug("[ICE] 跳过自己的 ID: {}", peer_id);
            continue;
        }
        
        // 检查是否已存在该 peer
        auto existing_it = m_peers.find(peer_id);
        if (existing_it != m_peers.end()) {
            auto existing_state = existing_it->second->get_state();
            
            // 【修复】如果 force=true（Tracker 重连场景），强制重新连接
            // 因为网络可能已经变化，旧的 ICE 连接无效
            if (!force && (existing_state == PeerState::Connected || 
                           existing_state == PeerState::Connecting)) {
                continue;  // 非强制模式下，已连接或正在连接的跳过
            }
            
            // 【修复】如果处于 Failed 或 Disconnected 状态，删除旧的并重新建立
            // 这修复了睡眠唤醒后无法重连的问题
            g_logger->info("[ICE] 对等点 {} 已存在但状态为 {}，重新建立连接", 
                          peer_id, static_cast<int>(existing_state));
            existing_it->second->close();
            m_peers.erase(existing_it);
            
            // 清除重连计数（给新连接一个干净的开始）
            {
                std::lock_guard<std::mutex> reconnect_lock(m_reconnect_mutex);
                m_reconnect_attempts.erase(peer_id);
                m_last_reconnect_time.erase(peer_id);
            }
        }

        g_logger->info("[ICE] 正在为对等点 {} 创建 PeerController...", peer_id);

        // 创建 PeerController 回调
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
            m_crypto,
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
            
            // 【修复】Answer 方添加等待超时机制
            // 如果 30 秒内没有收到 Offer，触发 Failed 状态启动重连
            auto timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
            timeout_timer->expires_after(std::chrono::seconds(30));
            timeout_timer->async_wait([weak_controller = std::weak_ptr<PeerController>(controller), 
                                       peer_id, 
                                       timeout_timer,
                                       self = shared_from_this()](const boost::system::error_code& ec) {
                if (ec) return;  // 定时器被取消
                
                auto ctrl = weak_controller.lock();
                if (!ctrl) return;
                
                // 如果仍然处于 Disconnected 状态（没收到 Offer），触发失败
                if (ctrl->get_state() == PeerState::Disconnected) {
                    g_logger->warn("[ICE] Answer 方等待 Offer 超时 (30s), 对等点: {}", peer_id);
                    self->handle_peer_state_changed(peer_id, PeerState::Failed);
                }
            });
        }
    }
}

// 新增：处理 Peer 状态变化
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
                    m_sync_session->perform_flood_sync(controller, session_id);
                });
            }
        }
    } else if (state == PeerState::Failed) {
        g_logger->warn("[ICE] ❌ 与 {} 连接失败，尝试重连...", peer_id);
        schedule_reconnect(peer_id);
    }
}

// 新增：处理 Peer 消息（来自 PeerController 的回调）
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
    
    // 转发到 PeerController 处理
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

// 广播给所有连接的对等点
void P2PManager::send_over_kcp(const std::string& msg) {
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    // 加密已下沉到 PeerController
    std::string& final_msg = json_packet; 

    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);  // 读操作
    int sent_count = 0;
    for (auto& [peer_id, controller] : m_peers) {
        if (controller->is_connected()) {
            controller->send_message(final_msg);
            sent_count++;
        }
    }
    if (sent_count > 0) {
        g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)", sent_count, final_msg.length());
    }
}
// 发给特定对等点
void P2PManager::send_over_kcp_peer(const std::string& msg, PeerController* peer) {
    if (!peer || !peer->is_connected()) {
        g_logger->warn("[KCP] 尝试向无效或未就绪的对等点发送消息。");
        return;
    }
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    // 加密已下沉到 PeerController
    std::string& final_msg = json_packet; 

    peer->send_message(final_msg);
}
// 通过 peer_id 安全发送（会在锁内查找）
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
    // 解密已下沉到 PeerController，这里收到的是明文
    const std::string& decrypted_msg = msg;

    if (decrypted_msg.empty()) return;

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
                m_sync_handler->handle_share_state(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE && can_receive) {
                m_sync_handler->handle_file_update(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_DELETE && can_receive) {
                m_sync_handler->handle_file_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_REQUEST_FILE) {
                bool can_serve = (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional);
                if (can_serve && from_peer) {
                    m_transfer_manager->queue_upload(from_peer->get_peer_id(), json_payload);
                }
            } else if (json_msg_type == Protocol::TYPE_DIR_CREATE && can_receive) {
                m_sync_handler->handle_dir_create(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_DIR_DELETE && can_receive) {
                m_sync_handler->handle_dir_delete(json_payload, from_peer);
            // --- 批量消息路由 (阶段1优化) ---
            } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE_BATCH && can_receive) {
                m_sync_handler->handle_file_update_batch(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_DELETE_BATCH && can_receive) {
                m_sync_handler->handle_file_delete_batch(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_DIR_BATCH && can_receive) {
                m_sync_handler->handle_dir_batch(json_payload, from_peer);
            // ---------------------------------
            } else if (json_msg_type == Protocol::TYPE_SYNC_BEGIN && can_receive) {
                m_sync_session->handle_sync_begin(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_SYNC_ACK) {
                m_sync_session->handle_sync_ack(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_GOODBYE) {
                // 【断点续传】处理对端正常退出通知
                handle_goodbye(from_peer);
            // --- 心跳保活 ---
            } else if (json_msg_type == Protocol::TYPE_HEARTBEAT) {
                handle_heartbeat(from_peer);
            } else if (json_msg_type == Protocol::TYPE_HEARTBEAT_ACK) {
                handle_heartbeat_ack(from_peer);
            // ----------------
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
            // 二进制文件块，交给 TransferManager 处理
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

std::vector<P2PManager::PeerInfo> P2PManager::get_peers_info() {
    std::vector<PeerInfo> result;
    
    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
    result.reserve(m_peers.size());
    
    for (const auto& [peer_id, controller] : m_peers) {
        PeerInfo info;
        info.peer_id = peer_id;
        info.connected_since = controller->connected_at_ts.load();
        
        // 转换 PeerState 到字符串
        switch (controller->get_state()) {
            case PeerState::Connected:
                info.state = "connected";
                break;
            case PeerState::Connecting:
                info.state = "connecting";
                break;
            case PeerState::Failed:
                info.state = "failed";
                break;
            case PeerState::Disconnected:
            default:
                info.state = "disconnected";
                break;
        }
        
        // 转换 IceConnectionType 到字符串
        switch (controller->get_connection_type()) {
            case IceConnectionType::Direct:
                info.connection_type = "direct";
                break;
            case IceConnectionType::Relay:
                info.connection_type = "relay";
                break;
            default:
                info.connection_type = "unknown";
                break;
        }
        
        result.push_back(std::move(info));
    }
    
    return result;
}

// ═══════════════════════════════════════════════════════════════
// 业务逻辑处理器（已迁移到 SyncHandler）
// ═══════════════════════════════════════════════════════════════

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
// UPnP（已迁移到 UpnpManager）
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
// 同步会话管理（已迁移到 SyncSession）
// ═══════════════════════════════════════════════════════════════

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
    // 加密已下沉到 PeerController
    std::string& final_msg = msg_str;
    
    std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
    int sent_count = 0;
    
    for (auto& [peer_id, controller] : m_peers) {
        if (controller->is_connected()) {
            controller->send_message(final_msg);
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

// ═══════════════════════════════════════════════════════════════
// 心跳保活机制
// ═══════════════════════════════════════════════════════════════

void P2PManager::schedule_heartbeat() {
    m_heartbeat_timer.expires_after(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    m_heartbeat_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->send_heartbeats();
            self->schedule_heartbeat();  // 重新调度
        }
    });
}

void P2PManager::send_heartbeats() {
    // 只在有连接的 peer 时发送心跳
    std::vector<std::shared_ptr<PeerController>> connected_peers;
    {
        std::shared_lock<std::shared_mutex> lock(m_peers_mutex);
        for (auto& [peer_id, controller] : m_peers) {
            if (controller->is_connected()) {
                connected_peers.push_back(controller);
            }
        }
    }
    
    if (connected_peers.empty()) {
        return;  // 没有连接的 peer，跳过心跳
    }
    
    // 构造心跳消息
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_HEARTBEAT;
    msg[Protocol::MSG_PAYLOAD] = {
        {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()}
    };
    
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg.dump());
    
    // 向所有连接的 peer 发送心跳
    for (auto& controller : connected_peers) {
        if (controller->is_valid()) {
            controller->send_message(json_packet);
        }
    }
    
    g_logger->debug("[Heartbeat] 发送心跳到 {} 个对等点", connected_peers.size());
}

void P2PManager::handle_heartbeat(PeerController* from_peer) {
    if (!from_peer) return;
    
    // 收到心跳请求，立即回复 ACK
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_HEARTBEAT_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()}
    };
    
    send_over_kcp_peer(msg.dump(), from_peer);
    
    g_logger->debug("[Heartbeat] 收到来自 {} 的心跳，已回复 ACK", from_peer->get_peer_id());
}

void P2PManager::handle_heartbeat_ack(PeerController* from_peer) {
    if (!from_peer) return;
    
    // 收到心跳响应，连接正常
    // 这里可以用于统计 RTT 或检测连接质量，目前只记录日志
    g_logger->debug("[Heartbeat] 收到来自 {} 的心跳 ACK", from_peer->get_peer_id());
}

}  // namespace VeritasSync
