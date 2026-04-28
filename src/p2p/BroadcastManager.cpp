#include "VeritasSync/p2p/BroadcastManager.h"

#include <algorithm>
#include <thread>

#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 常量
// ═══════════════════════════════════════════════════════════════

static constexpr size_t FILE_UPDATE_BATCH_SIZE = 50;   // 每批最多 50 个文件更新
static constexpr size_t FILE_DELETE_BATCH_SIZE = 100;  // 每批最多 100 个文件删除

// 广播流控配置（与 SyncSession::perform_flood_sync 保持一致的阈值）
static constexpr int    BROADCAST_FLOW_CONTROL_THRESHOLD = 1024;  // KCP 积压量阈值
static constexpr int    BROADCAST_FLOW_CONTROL_SLEEP_MS  = 20;    // 积压时等待间隔
static constexpr int    BROADCAST_FLOW_CONTROL_MAX_WAIT  = 250;   // 单次最大等待轮次

// ═══════════════════════════════════════════════════════════════
// 构造 / 配置
// ═══════════════════════════════════════════════════════════════

BroadcastManager::BroadcastManager(boost::asio::io_context& io_context,
                                   boost::asio::thread_pool& worker_pool,
                                   PeerRegistry& peer_registry,
                                   SendCallback send_fn,
                                   FloodSyncCallback flood_sync_fn,
                                   StateProvider state_provider)
    : m_io_context(io_context),
      m_worker_pool(worker_pool),
      m_peer_registry(peer_registry),
      m_send_fn(std::move(send_fn)),
      m_flood_sync_fn(std::move(flood_sync_fn)),
      m_state_provider(std::move(state_provider)),
      m_reconciliation_timer(std::make_unique<boost::asio::steady_timer>(io_context)) {
}

void BroadcastManager::set_role(SyncRole role) { m_role = role; }
void BroadcastManager::set_mode(SyncMode mode) { m_mode = mode; }

bool BroadcastManager::can_broadcast() const {
    return m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional;
}

void BroadcastManager::stop() {
    if (m_reconciliation_timer) {
        m_reconciliation_timer->cancel();
    }
}

// ═══════════════════════════════════════════════════════════════
// 流控发送
// ═══════════════════════════════════════════════════════════════

bool BroadcastManager::send_to_peers_with_flow_control(
    const std::vector<std::shared_ptr<PeerController>>& peers,
    const std::string& packet) {
    bool all_ok = true;
    for (auto& controller : peers) {
        int wait_count = 0;
        int pending = controller->get_kcp_wait_send();
        while (pending > BROADCAST_FLOW_CONTROL_THRESHOLD &&
               controller->is_valid() &&
               wait_count < BROADCAST_FLOW_CONTROL_MAX_WAIT) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(BROADCAST_FLOW_CONTROL_SLEEP_MS));
            pending = controller->get_kcp_wait_send();
            wait_count++;
        }

        if (controller->send_message(packet) < 0) {
            all_ok = false;
        }
    }

    if (!peers.empty()) {
        g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)",
                      peers.size(), packet.length());
    }
    return all_ok;
}

// ═══════════════════════════════════════════════════════════════
// 单条广播
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::broadcast_current_state() {
    if (!can_broadcast()) return;
    if (!m_state_provider) return;

    boost::asio::post(m_worker_pool, [this]() {
        std::string json_state = m_state_provider();

        std::string json_packet;
        json_packet.push_back(MSG_TYPE_JSON);
        json_packet.append(json_state);

        boost::asio::post(m_io_context, [this, json_packet = std::move(json_packet)]() {
            auto peers = m_peer_registry.collect_connected();
            for (auto& controller : peers) {
                controller->send_message(json_packet);
            }
            if (!peers.empty()) {
                g_logger->info("[P2P] (Source) 广播状态完成 (发送给 {} 个对等点)", peers.size());
            }
        });
    });
}

void BroadcastManager::broadcast_file_update(const FileInfo& file_info) {
    if (!can_broadcast()) return;
    g_logger->info("[P2P] (Source) 广播增量更新: {}", file_info.path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
    msg[Protocol::MSG_PAYLOAD] = file_info;
    if (!m_send_fn(msg.dump())) {
        g_logger->warn("[P2P] 广播文件更新失败（无可用对等点）: {}", file_info.path);
    }
}

// --- 通用路径广播辅助（消除 3 个函数的重复结构）---
static bool broadcast_path_event(SyncRole role, SyncMode mode,
                                 const std::string& msg_type,
                                 const std::string& log_label,
                                 const std::string& relative_path,
                                 std::function<bool(const std::string&)> send_fn) {
    if (!(role == SyncRole::Source || mode == SyncMode::BiDirectional)) return false;
    g_logger->info("[P2P] (Source) 广播增量{}: {}", log_label, relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = msg_type;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    bool ok = send_fn(msg.dump());
    if (!ok) {
        g_logger->warn("[P2P] 广播{}失败（无可用对等点）: {}", log_label, relative_path);
    }
    return ok;
}

void BroadcastManager::broadcast_file_delete(const std::string& relative_path) {
    broadcast_path_event(m_role, m_mode, Protocol::TYPE_FILE_DELETE, "删除", relative_path,
                         [this](const std::string& s){ return m_send_fn(s); });
}

void BroadcastManager::broadcast_dir_create(const std::string& relative_path) {
    broadcast_path_event(m_role, m_mode, Protocol::TYPE_DIR_CREATE, "目录创建", relative_path,
                         [this](const std::string& s){ return m_send_fn(s); });
}

void BroadcastManager::broadcast_dir_delete(const std::string& relative_path) {
    broadcast_path_event(m_role, m_mode, Protocol::TYPE_DIR_DELETE, "目录删除", relative_path,
                         [this](const std::string& s){ return m_send_fn(s); });
}

// ═══════════════════════════════════════════════════════════════
// 批量广播
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::broadcast_file_updates_batch(const std::vector<FileInfo>& files) {
    if (!can_broadcast()) return;
    if (files.empty()) return;

    boost::asio::post(m_worker_pool, [this, files]() {
        g_logger->info("[P2P] (Source) 批量广播 {} 个文件更新", files.size());

        bool any_failed = false;
        for (size_t i = 0; i < files.size(); i += FILE_UPDATE_BATCH_SIZE) {
            size_t end = std::min(i + FILE_UPDATE_BATCH_SIZE, files.size());

            nlohmann::json msg;
            msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE_BATCH;
            msg[Protocol::MSG_PAYLOAD]["files"] = nlohmann::json::array();

            for (size_t j = i; j < end; ++j) {
                msg[Protocol::MSG_PAYLOAD]["files"].push_back(files[j]);
            }

            std::string json_packet;
            json_packet.push_back(MSG_TYPE_JSON);
            json_packet.append(msg.dump());

            auto peers = m_peer_registry.collect_connected();
            if (!send_to_peers_with_flow_control(peers, json_packet)) {
                any_failed = true;
            }

            g_logger->debug("[P2P] 发送文件更新批次 {}/{} ({} 个文件)",
                           (i / FILE_UPDATE_BATCH_SIZE) + 1,
                           (files.size() + FILE_UPDATE_BATCH_SIZE - 1) / FILE_UPDATE_BATCH_SIZE,
                           end - i);
        }

        if (any_failed) {
            g_logger->warn("[P2P] 部分文件更新批次发送失败（无可用对等点）");
        }

        schedule_reconciliation();
    });
}

void BroadcastManager::broadcast_file_deletes_batch(const std::vector<std::string>& paths) {
    if (!can_broadcast()) return;
    if (paths.empty()) return;

    boost::asio::post(m_worker_pool, [this, paths]() {
        g_logger->info("[P2P] (Source) 批量广播 {} 个文件删除", paths.size());

        bool any_failed = false;
        for (size_t i = 0; i < paths.size(); i += FILE_DELETE_BATCH_SIZE) {
            size_t end = std::min(i + FILE_DELETE_BATCH_SIZE, paths.size());

            nlohmann::json msg;
            msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE_BATCH;
            msg[Protocol::MSG_PAYLOAD]["paths"] = nlohmann::json::array();

            for (size_t j = i; j < end; ++j) {
                msg[Protocol::MSG_PAYLOAD]["paths"].push_back(paths[j]);
            }

            std::string json_packet;
            json_packet.push_back(MSG_TYPE_JSON);
            json_packet.append(msg.dump());

            auto peers = m_peer_registry.collect_connected();
            if (!send_to_peers_with_flow_control(peers, json_packet)) {
                any_failed = true;
            }
        }

        if (any_failed) {
            g_logger->warn("[P2P] 部分文件删除批次发送失败（无可用对等点）");
        }

        schedule_reconciliation();
    });
}

void BroadcastManager::broadcast_dir_changes_batch(const std::vector<std::string>& creates,
                                                    const std::vector<std::string>& deletes) {
    if (!can_broadcast()) return;
    if (creates.empty() && deletes.empty()) return;

    boost::asio::post(m_worker_pool, [this, creates, deletes]() {
        g_logger->info("[P2P] (Source) 批量广播目录变更: {} 创建, {} 删除",
                       creates.size(), deletes.size());

        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_BATCH;
        msg[Protocol::MSG_PAYLOAD]["creates"] = creates;
        msg[Protocol::MSG_PAYLOAD]["deletes"] = deletes;

        std::string json_packet;
        json_packet.push_back(MSG_TYPE_JSON);
        json_packet.append(msg.dump());

        auto peers = m_peer_registry.collect_connected();
        send_to_peers_with_flow_control(peers, json_packet);

        schedule_reconciliation();
    });
}

// ═══════════════════════════════════════════════════════════════
// 优雅关闭
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::broadcast_goodbye() {
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_GOODBYE;
    msg[Protocol::MSG_PAYLOAD] = nlohmann::json::object();

    std::string msg_str;
    msg_str.push_back(MSG_TYPE_JSON);
    msg_str.append(msg.dump());

    auto peers = m_peer_registry.collect_connected();

    for (auto& controller : peers) {
        controller->send_message(msg_str);
        controller->flush_kcp();  // 强制刷新
        g_logger->info("[P2P] 向 {} 发送 goodbye", controller->get_peer_id());
    }

    g_logger->info("[P2P] goodbye 已发送给 {} 个对等点", peers.size());
}

// ═══════════════════════════════════════════════════════════════
// Anti-Entropy 对账
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::schedule_reconciliation() {
    if (!m_reconciliation_timer) return;
    if (!can_broadcast()) return;

    boost::asio::post(m_io_context, [this]() {
        m_reconciliation_timer->cancel();
        m_reconciliation_timer->expires_after(std::chrono::seconds(RECONCILIATION_DELAY_SECONDS));
        m_reconciliation_timer->async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                trigger_reconciliation();
            }
        });
    });
}

void BroadcastManager::trigger_reconciliation() {
    auto peers = m_peer_registry.collect_connected();
    if (peers.empty()) return;

    g_logger->info("[Anti-Entropy] 静默期 {}s 已过，开始对账 (flood sync) 覆盖 {} 个对等点",
                   RECONCILIATION_DELAY_SECONDS, peers.size());

    for (auto& controller : peers) {
        uint64_t session_id = std::chrono::steady_clock::now().time_since_epoch().count();
        controller->set_sync_session_id(session_id);

        std::string peer_id = controller->get_peer_id();
        boost::asio::post(m_worker_pool, [this, controller, session_id, peer_id]() {
            g_logger->info("[Anti-Entropy] 开始向 {} 执行对账 flood sync (session={})", peer_id, session_id);
            m_flood_sync_fn(controller, session_id);
            g_logger->info("[Anti-Entropy] 对账 flood sync 完成 (peer={}, session={})", peer_id, session_id);
        });
    }
}

}  // namespace VeritasSync
