#include "VeritasSync/p2p/P2PManager.h"

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <thread>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/p2p/TrackerClient.h"

// 前向声明对应的完整定义
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/sync/SyncHandler.h"
#include "VeritasSync/sync/SyncSession.h"

// 子组件
#include "VeritasSync/p2p/KcpScheduler.h"
#include "VeritasSync/p2p/BroadcastManager.h"

namespace VeritasSync {

// 命名常量
static constexpr int      ANSWER_WAIT_TIMEOUT_SECONDS= 30;
static constexpr int      CLEANUP_INTERVAL_MINUTES   = 5;
static constexpr int      KCP_FLUSH_POLL_MS          = 20;

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

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
    m_crypto->set_key(key_string);
}

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

// ═══════════════════════════════════════════════════════════════
// 广播方法（委托给 BroadcastManager）
// ═══════════════════════════════════════════════════════════════

void P2PManager::broadcast_current_state()       { m_broadcast_manager->broadcast_current_state(); }
void P2PManager::broadcast_file_update(const FileInfo& file_info) { m_broadcast_manager->broadcast_file_update(file_info); }
void P2PManager::broadcast_file_delete(const std::string& relative_path) { m_broadcast_manager->broadcast_file_delete(relative_path); }
void P2PManager::broadcast_dir_create(const std::string& relative_path) { m_broadcast_manager->broadcast_dir_create(relative_path); }
void P2PManager::broadcast_dir_delete(const std::string& relative_path) { m_broadcast_manager->broadcast_dir_delete(relative_path); }
void P2PManager::broadcast_file_updates_batch(const std::vector<FileInfo>& files) { m_broadcast_manager->broadcast_file_updates_batch(files); }
void P2PManager::broadcast_file_deletes_batch(const std::vector<std::string>& paths) { m_broadcast_manager->broadcast_file_deletes_batch(paths); }
void P2PManager::broadcast_dir_changes_batch(const std::vector<std::string>& creates,
                                             const std::vector<std::string>& deletes) {
    m_broadcast_manager->broadcast_dir_changes_batch(creates, deletes);
}

// ═══════════════════════════════════════════════════════════════
// 静态工厂与构造/析构
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<P2PManager> P2PManager::create(const P2PManagerConfig& config) {
    struct P2PManagerMaker : public P2PManager {
        P2PManagerMaker() : P2PManager() {}
    };
    auto manager = std::make_shared<P2PManagerMaker>();
    // 从 config 初始化所有参数（含 STUN/TURN）
    manager->m_chunk_size = config.chunk_size;
    manager->m_kcp_window_size = config.kcp_window_size;
    manager->m_kcp_update_interval_ms = config.kcp_update_interval_ms;
    manager->m_stun_host = config.stun_host;
    manager->m_stun_port = config.stun_port;
    manager->m_turn_host = config.turn_host;
    manager->m_turn_port = config.turn_port;
    manager->m_turn_username = config.turn_username;
    manager->m_turn_password = config.turn_password;

    if (!config.stun_host.empty()) {
        g_logger->info("[Config] STUN 服务器设置为: {}:{}", config.stun_host, config.stun_port);
    }
    if (!config.turn_host.empty()) {
        g_logger->info("[Config] TURN 服务器设置为: {}:{}", config.turn_host, config.turn_port);
    }

    manager->init();
    return manager;
}

P2PManager::P2PManager()
    : m_io_context(),
      m_crypto(std::make_shared<CryptoLayer>()),
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

void P2PManager::set_tracker_client(TrackerClient* tc) {
    m_tracker_client = tc;
}
void P2PManager::set_role(SyncRole role) {
    m_role = role;
    if (m_sync_handler) m_sync_handler->set_role(role);
    if (m_sync_session) m_sync_session->set_role(role);
    if (m_broadcast_manager) m_broadcast_manager->set_role(role);
}

void P2PManager::set_mode(SyncMode mode) {
    m_mode = mode;
    if (m_sync_handler) m_sync_handler->set_mode(mode);
    if (m_sync_session) m_sync_session->set_mode(mode);
    if (m_broadcast_manager) m_broadcast_manager->set_mode(mode);
}

// --- 初始化子方法 ---

void P2PManager::create_transfer_manager() {
    auto send_cb = [weak_self = weak_from_this()](const std::string& peer_id,
                                                  const std::string& encrypted_data) -> int {
        auto self = weak_self.lock();
        if (!self) return 0;

        auto controller = self->m_peer_registry.find(peer_id);

        if (controller && controller->is_connected() && controller->is_valid()) {
            return controller->send_message(encrypted_data);
        }
        return -1;
    };
    m_transfer_manager = std::make_shared<TransferManager>(m_state_manager, m_io_context, m_worker_pool, send_cb, m_chunk_size);
}

void P2PManager::create_sync_components() {
    auto with_peer_cb = [this](const std::string& peer_id, std::function<void(PeerController*)> action) {
        auto controller = m_peer_registry.find(peer_id);
        if (controller && controller->is_connected()) {
            action(controller.get());
        }
    };

    m_sync_handler = std::make_unique<SyncHandler>(
        m_state_manager,
        m_transfer_manager,
        m_worker_pool,
        m_io_context,
        [this](const std::string& msg, PeerController* peer) {
            send_over_kcp_peer(msg, peer);
        },
        [this](const std::string& msg, const std::string& peer_id) {
            send_over_kcp_peer_safe(msg, peer_id);
        },
        with_peer_cb
    );
    m_sync_handler->set_role(m_role);
    m_sync_handler->set_mode(m_mode);

    m_sync_session = std::make_unique<SyncSession>(
        m_state_manager,
        m_worker_pool,
        m_io_context,
        [this](const std::string& msg, PeerController* peer) {
            send_over_kcp_peer(msg, peer);
        },
        with_peer_cb,
        [this](const std::string& peer_id) -> std::shared_ptr<PeerController> {
            return m_peer_registry.find(peer_id);
        }
    );
    m_sync_session->set_role(m_role);
    m_sync_session->set_mode(m_mode);
}

void P2PManager::start_background_services() {
    m_thread = std::jthread([this]() {
        g_logger->info("[P2P] IO context 在后台线程运行...");
        auto work_guard = boost::asio::make_work_guard(m_io_context);
        m_io_context.run();
    });
    m_kcp_scheduler->start();
    schedule_cleanup_task();
    m_upnp.init_async(m_worker_pool);
}

void P2PManager::init() {
    create_transfer_manager();
    create_sync_components();

    auto collect_peers_cb = [this]() { return m_peer_registry.collect_connected(); };
    m_kcp_scheduler = std::make_unique<KcpScheduler>(m_io_context, collect_peers_cb, m_kcp_update_interval_ms);

    // 创建 BroadcastManager
    m_broadcast_manager = std::make_unique<BroadcastManager>(
        m_io_context,
        m_worker_pool,
        m_peer_registry,
        // send_fn: 广播消息给所有已连接对等点
        [this](const std::string& msg) -> bool { return send_over_kcp(msg); },
        // flood_sync_fn: 对账时调用 SyncSession::perform_flood_sync
        [this](std::shared_ptr<PeerController> peer, uint64_t session_id) {
            m_sync_session->perform_flood_sync(peer, session_id);
        },
        // state_provider: 获取当前目录状态 JSON
        [this]() -> std::string {
            if (!m_state_manager) return "";
            m_state_manager->scan_directory();
            return m_state_manager->get_state_as_json_string();
        }
    );
    m_broadcast_manager->set_role(m_role);
    m_broadcast_manager->set_mode(m_mode);

    // 注册消息路由
    register_message_handlers();

    start_background_services();
}

P2PManager::~P2PManager() {
    if (m_kcp_scheduler) m_kcp_scheduler->stop();
    if (m_broadcast_manager) m_broadcast_manager->stop();

    m_peer_registry.close_all();

    m_io_context.stop();
    m_worker_pool.join();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// ═══════════════════════════════════════════════════════════════
// 连接管理 - 使用 PeerController
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<PeerController> P2PManager::create_peer_controller(
        const std::string& self_id, const std::string& peer_id) {
    PeerControllerCallbacks callbacks;

    callbacks.on_state_changed = [this, peer_id](PeerState state) {
        handle_peer_state_changed(peer_id, state);
    };

    callbacks.on_signal_needed = [this, peer_id](const std::string& signal_type, const std::string& payload) {
        if (m_tracker_client) {
            m_tracker_client->send_signaling_message(peer_id, signal_type, payload);
        }
    };

    callbacks.on_message_received = [this, peer_id](const std::string& message) {
        handle_peer_message(peer_id, message);
    };

    KcpConfig kcp_config;
    kcp_config.snd_wnd = static_cast<int>(m_kcp_window_size);
    kcp_config.rcv_wnd = static_cast<int>(m_kcp_window_size);

    return PeerController::create(
        self_id,
        peer_id,
        m_io_context,
        create_ice_config(),
        m_crypto,
        std::move(callbacks),
        kcp_config
    );
}

void P2PManager::setup_answer_timeout(std::shared_ptr<PeerController> controller,
                                      const std::string& peer_id) {
    auto timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    timeout_timer->expires_after(std::chrono::seconds(ANSWER_WAIT_TIMEOUT_SECONDS));
    timeout_timer->async_wait([weak_controller = std::weak_ptr<PeerController>(controller),
                               peer_id,
                               self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec) return;

        auto ctrl = weak_controller.lock();
        if (!ctrl) return;

        if (ctrl->get_state() == PeerState::Disconnected) {
            g_logger->warn("[ICE] Answer 方等待 Offer 超时 (30s), 对等点: {}", peer_id);
            self->handle_peer_state_changed(peer_id, PeerState::Failed);
        }
    });
}

void P2PManager::connect_to_peers(const std::vector<std::string>& peer_addresses, bool force) {
    if (!m_tracker_client) {
        g_logger->error("[ICE] TrackerClient is null, 无法获取 self_id。");
        return;
    }
    std::string self_id = m_tracker_client->get_self_id();
    if (self_id.empty()) {
        g_logger->warn("[ICE] Self ID 尚未设置，推迟连接逻辑。");
        return;
    }

    std::vector<std::string> peers_to_create;
    for (const auto& peer_id : peer_addresses) {
        if (peer_id == self_id) {
            g_logger->debug("[ICE] 跳过自己的 ID: {}", peer_id);
            continue;
        }
        if (m_peer_registry.try_reuse_or_evict(peer_id, force)) {
            continue;
        }
        peers_to_create.push_back(peer_id);
    }

    if (peers_to_create.empty()) return;

    struct NewPeer {
        std::string peer_id;
        std::shared_ptr<PeerController> controller;
    };
    std::vector<NewPeer> new_peers;
    new_peers.reserve(peers_to_create.size());

    for (const auto& peer_id : peers_to_create) {
        g_logger->info("[ICE] 正在为对等点 {} 创建 PeerController...", peer_id);
        auto controller = create_peer_controller(self_id, peer_id);
        if (!controller) {
            g_logger->error("[ICE] PeerController::create 失败 (对等点: {})", peer_id);
            continue;
        }
        new_peers.push_back({peer_id, std::move(controller)});
    }

    for (auto& np : new_peers) {
        if (m_peer_registry.find(np.peer_id)) {
            g_logger->debug("[ICE] {} 已被并发创建，跳过", np.peer_id);
            np.controller->close();
            np.controller.reset();
            continue;
        }
        m_peer_registry.add(np.peer_id, np.controller);
    }

    for (auto& np : new_peers) {
        if (!np.controller || !np.controller->is_valid()) continue;

        if (np.controller->is_offer_side()) {
            g_logger->info("[ICE] 我们是 Offer 方，主动发起连接 (对于 {})", np.peer_id);
            np.controller->initiate_connection();
        } else {
            g_logger->info("[ICE] 我们是 Answer 方，等待 Offer (对于 {})", np.peer_id);
            setup_answer_timeout(np.controller, np.peer_id);
        }
    }
}

void P2PManager::handle_peer_state_changed(const std::string& peer_id, PeerState state) {
    g_logger->info("[ICE] Peer {} 状态变化: {}", peer_id, static_cast<int>(state));

    if (state == PeerState::Connected) {
        g_logger->info("[ICE] ✅ 与 {} 建立连接成功！", peer_id);

        if (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional) {
            auto controller = m_peer_registry.find(peer_id);

            if (controller) {
                if (m_transfer_manager) {
                    auto pending = m_transfer_manager->get_pending_receives_for_peer(peer_id);
                    if (!pending.empty()) {
                        g_logger->info("[P2P] 连接恢复，发现 {} 个未完成的接收任务 (peer: {})，"
                                       "将通过 flood sync 重新评估",
                                       pending.size(), peer_id);
                    }
                }

                g_logger->info("[P2P] 连接激活，准备向对等点 {} 推送文件状态...", peer_id);

                uint64_t session_id = std::chrono::steady_clock::now().time_since_epoch().count();
                controller->set_sync_session_id(session_id);

                boost::asio::post(m_worker_pool, [this, self = shared_from_this(), controller, session_id]() {
                    m_sync_session->perform_flood_sync(controller, session_id);
                });
            }
        }
    } else if (state == PeerState::Failed) {
        g_logger->warn("[ICE] ❌ 与 {} 连接失败 (ICE+TURN 均不可达)", peer_id);
    }
}

void P2PManager::handle_peer_message(const std::string& peer_id, const std::string& encrypted_msg) {
    auto controller = m_peer_registry.find(peer_id);
    if (!controller) {
        g_logger->warn("[KCP] 收到来自未知对等点 {} 的消息", peer_id);
        return;
    }

    handle_kcp_message(encrypted_msg, controller.get());
}

// ═══════════════════════════════════════════════════════════════
// 信令处理
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_signaling_message(const std::string& from_peer_id,
                                          const std::string& message_type,
                                          const std::string& payload) {
    auto controller = m_peer_registry.find(from_peer_id);
    if (!controller) {
        g_logger->warn("[ICE] 收到来自未知对等点 {} 的信令消息。", from_peer_id);
        return;
    }

    if (message_type == "ice_candidate") {
        g_logger->debug("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    } else {
        g_logger->info("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    }

    controller->handle_signaling(message_type, payload);
}

// ═══════════════════════════════════════════════════════════════
// Peer 断开/重连
// ═══════════════════════════════════════════════════════════════

void P2PManager::handle_peer_leave(const std::string& peer_id) {
    auto controller = m_peer_registry.find(peer_id);
    if (!controller) {
        g_logger->debug("[P2P] peer_leave: {} 已不在连接池中", peer_id);
        return;
    }

    if (controller->is_graceful_shutdown()) {
        g_logger->debug("[P2P] peer_leave: {} 是主动退出（已收到 goodbye），跳过清理", peer_id);
    } else {
        g_logger->info("[P2P] {} 掉线（未收到 goodbye），保留传输状态等待续传", peer_id);
    }

    controller->close();
    m_peer_registry.remove(peer_id);
}

// ═══════════════════════════════════════════════════════════════
// 消息发送
// ═══════════════════════════════════════════════════════════════

bool P2PManager::send_over_kcp(const std::string& msg) {
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);

    auto peers = m_peer_registry.collect_connected();
    bool any_sent = false;
    for (auto& controller : peers) {
        if (controller->send_message(json_packet) >= 0) {
            any_sent = true;
        }
    }
    if (!peers.empty()) {
        g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)", peers.size(), json_packet.length());
    }
    return any_sent;
}

void P2PManager::send_over_kcp_peer(const std::string& msg, PeerController* peer) {
    if (!peer || !peer->is_connected()) {
        g_logger->warn("[KCP] 尝试向无效或未就绪的对等点发送消息。");
        return;
    }
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);

    peer->send_message(json_packet);
}

void P2PManager::send_over_kcp_peer_safe(const std::string& msg, const std::string& peer_id) {
    auto controller = m_peer_registry.find(peer_id);
    if (controller && controller->is_connected()) {
        send_over_kcp_peer(msg, controller.get());
    }
}

// ═══════════════════════════════════════════════════════════════
// 消息处理 - 核心路由
// ═══════════════════════════════════════════════════════════════

void P2PManager::register_message_handlers() {
    auto& router = m_message_router;

    router.register_handler(Protocol::TYPE_SHARE_STATE,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_share_state(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_FILE_UPDATE,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_file_update(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_FILE_DELETE,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_file_delete(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_REQUEST_FILE,
        [this](nlohmann::json& payload, PeerController* peer) {
            bool can_serve = (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional);
            if (can_serve && peer) {
                m_transfer_manager->queue_upload(peer->get_peer_id(), payload);
            }
        }, false);

    router.register_handler(Protocol::TYPE_DIR_CREATE,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_dir_create(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_DIR_DELETE,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_dir_delete(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_FILE_UPDATE_BATCH,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_file_update_batch(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_FILE_DELETE_BATCH,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_file_delete_batch(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_DIR_BATCH,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_handler->handle_dir_batch(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_SYNC_BEGIN,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_session->handle_sync_begin(payload, peer);
        }, true);

    router.register_handler(Protocol::TYPE_SYNC_ACK,
        [this](nlohmann::json& payload, PeerController* peer) {
            m_sync_session->handle_sync_ack(payload, peer);
        }, false);

    router.register_handler(Protocol::TYPE_GOODBYE,
        [this](nlohmann::json& /*payload*/, PeerController* peer) {
            handle_goodbye(peer);
        }, false);
}

void P2PManager::handle_kcp_message(const std::string& msg, PeerController* from_peer) {
    if (msg.empty()) return;

    uint8_t msg_type = msg[0];
    std::string payload(msg.begin() + 1, msg.end());

    bool can_receive = m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional;
    if (msg_type == MSG_TYPE_JSON) {
        try {
            auto json = nlohmann::json::parse(payload);
            const std::string json_msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
            auto& json_payload = json.at(Protocol::MSG_PAYLOAD);

            g_logger->info("[KCP] 收到 '{}' 消息 (来自: {})", json_msg_type,
                           from_peer ? from_peer->get_peer_id() : "<unknown>");

            if (!m_message_router.dispatch(json_msg_type, json_payload, from_peer, can_receive)) {
                g_logger->warn("[KCP] 消息类型 '{}' 不适用于当前角色 ({})", json_msg_type,
                               m_role == SyncRole::Source ? "Source" : "Destination");
            }
        } catch (const std::exception& e) {
            g_logger->error("[P2P] 处理KCP JSON消息时发生错误: {}", e.what());
        }
    } else if (msg_type == MSG_TYPE_BINARY_CHUNK) {
        if (can_receive) {
            std::string sender_id = from_peer ? from_peer->get_peer_id() : "";
            m_transfer_manager->handle_chunk(std::move(payload), sender_id);
        }
    } else if (msg_type == MSG_TYPE_PING) {
        // 心跳包
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
    auto controllers = m_peer_registry.collect_all();

    std::vector<PeerInfo> result;
    result.reserve(controllers.size());

    for (const auto& controller : controllers) {
        PeerInfo info;
        info.peer_id = controller->get_peer_id();
        info.connected_since = controller->get_connected_at_ts();

        switch (controller->get_state()) {
            case PeerState::Connected:  info.state = "connected"; break;
            case PeerState::Connecting: info.state = "connecting"; break;
            case PeerState::Failed:     info.state = "failed"; break;
            case PeerState::Disconnected:
            default:                    info.state = "disconnected"; break;
        }

        switch (controller->get_connection_type()) {
            case IceConnectionType::Direct: info.connection_type = "direct"; break;
            case IceConnectionType::Relay:  info.connection_type = "relay (TURN)"; break;
            default:                        info.connection_type = "unknown"; break;
        }

        result.push_back(std::move(info));
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// 文件传输相关
// ═══════════════════════════════════════════════════════════════

void P2PManager::schedule_cleanup_task() {
    m_cleanup_timer.expires_after(std::chrono::minutes(CLEANUP_INTERVAL_MINUTES));
    std::weak_ptr<P2PManager> weak_self = shared_from_this();
    m_cleanup_timer.async_wait([weak_self](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }

        if (auto self = weak_self.lock()) {
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
// 断点续传相关方法
// ═══════════════════════════════════════════════════════════════

void P2PManager::shutdown_gracefully() {
    g_logger->info("[P2P] 正在进行优雅关闭...");

    m_broadcast_manager->broadcast_goodbye();
    wait_for_kcp_flush();

    g_logger->info("[P2P] 优雅关闭完成");
}

void P2PManager::wait_for_kcp_flush(int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bool all_flushed = true;

        m_peer_registry.for_each([&](const std::string&, const std::shared_ptr<PeerController>& controller) {
            if (controller->get_kcp_wait_send() > 0) {
                all_flushed = false;
            }
        });

        if (all_flushed) {
            g_logger->debug("[P2P] 所有 KCP 发送队列已清空");
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(KCP_FLUSH_POLL_MS));
    }

    g_logger->warn("[P2P] KCP flush 超时，部分消息可能未送达");
}

void P2PManager::handle_goodbye(PeerController* from_peer) {
    if (!from_peer) return;

    std::string peer_id = from_peer->get_peer_id();
    g_logger->info("[P2P] 收到来自 {} 的 goodbye（程序正常关闭）", peer_id);

    from_peer->set_graceful_shutdown(true);

    if (m_transfer_manager) {
        m_transfer_manager->cancel_receives_for_peer(peer_id);
    }

    auto controller = m_peer_registry.remove(peer_id);
    if (controller) {
        controller->close();
    }
}

}  // namespace VeritasSync
