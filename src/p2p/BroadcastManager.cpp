#include "VeritasSync/p2p/BroadcastManager.h"

#include <algorithm>
#include <chrono>
#include <set>

#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/net/KcpProto.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 常量
// ═══════════════════════════════════════════════════════════════

static constexpr size_t FILE_UPDATE_BATCH_SIZE = 50;   // 每批最多 50 个文件更新
static constexpr size_t FILE_DELETE_BATCH_SIZE = 100;  // 每批最多 100 个文件删除

// ═══════════════════════════════════════════════════════════════
// 构造 / 配置
// ═══════════════════════════════════════════════════════════════

BroadcastManager::BroadcastManager(boost::asio::io_context& io_context,
                                   boost::asio::thread_pool& worker_pool,
                                   PeerRegistry& peer_registry,
                                   StateManager* state_manager,
                                   SendCallback send_fn,
                                   SendToPeerFunc send_to_peer)
    : m_io_context(io_context),
      m_worker_pool(worker_pool),
      m_peer_registry(peer_registry),
      m_state_manager(state_manager),
      m_send_fn(std::move(send_fn)),
      m_send_to_peer(std::move(send_to_peer)),
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

void BroadcastManager::send_to_peers_with_flow_control(
    const std::vector<std::shared_ptr<PeerController>>& peers,
    const std::string& packet) {
    if (peers.empty()) return;

    boost::asio::post(m_io_context,
        [peers, packet]() {
            g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)",
                          peers.size(), packet.length());

            struct State {
                std::vector<std::shared_ptr<PeerController>> peers;
                std::string packet;
                size_t index = 0;
            };
            auto state = std::make_shared<State>(State{peers, packet, 0});

            // shared_ptr<function> 解决递归 lambda 自引用生命周期问题
            auto send_next = std::make_shared<std::function<void()>>();
            *send_next = [state, send_next]() {
                if (state->index >= state->peers.size()) return;
                auto ctrl = state->peers[state->index];  // 按值捕获 shared_ptr
                if (!ctrl->is_valid()) {
                    state->index++;
                    (*send_next)();
                    return;
                }
                ctrl->on_send_ready([ctrl, state, send_next]() {
                    ctrl->send_message(state->packet);
                    state->index++;
                    (*send_next)();
                });
            };
            (*send_next)();
        });
}

// ═══════════════════════════════════════════════════════════════
// 单条广播
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::broadcast_current_state() {
    if (!can_broadcast()) return;
    if (!m_state_manager) return;

    boost::asio::post(m_worker_pool, [this]() {
        m_state_manager->scan_directory();
        std::string json_state = m_state_manager->get_state_as_json_string();

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
            send_to_peers_with_flow_control(peers, json_packet);
        }

        schedule_reconciliation();
    });
}

void BroadcastManager::broadcast_file_deletes_batch(const std::vector<std::string>& paths) {
    if (!can_broadcast()) return;
    if (paths.empty()) return;

    boost::asio::post(m_worker_pool, [this, paths]() {
        g_logger->info("[P2P] (Source) 批量广播 {} 个文件删除", paths.size());

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
            send_to_peers_with_flow_control(peers, json_packet);
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
// 全量推送（flood sync）
// ═══════════════════════════════════════════════════════════════

void BroadcastManager::perform_flood_sync(std::shared_ptr<PeerController> controller,
                                          uint64_t session_id) {
    if (!controller || !controller->is_valid() || !m_state_manager) {
        g_logger->warn("[Broadcast] perform_flood_sync: 上下文无效，跳过");
        return;
    }

    std::string peer_id = controller->get_peer_id();

    if (controller->get_sync_session_id() != session_id) {
        g_logger->info("[Broadcast] 会话 ID 已变更，跳过本次同步");
        return;
    }

    // 1. 扫描目录获取所有文件
    m_state_manager->scan_directory();
    std::vector<FileInfo> files = m_state_manager->get_all_files();
    auto dirs = m_state_manager->get_local_directories();

    if (files.empty() && dirs.empty()) {
        g_logger->info("[Broadcast] 没有文件需要推送给 {}", peer_id);
        return;
    }

    g_logger->info("[Broadcast] 开始向 {} 推送 {} 个文件和 {} 个目录 (session: {})...",
                   peer_id, files.size(), dirs.size(), session_id);

    // 2-4 全部在 io_context 上串行执行，文件发送使用异步背压
    boost::asio::post(m_io_context, [this, controller, session_id,
                                     files = std::move(files),
                                     dirs = std::move(dirs)]() {
        if (!controller->is_valid()) return;

        std::string peer_id = controller->get_peer_id();

        // 2. sync_begin
        nlohmann::json begin_msg;
        begin_msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_BEGIN;
        begin_msg[Protocol::MSG_PAYLOAD] = {
            {"session_id", session_id},
            {"file_count", files.size()},
            {"dir_count", dirs.size()}
        };
        m_send_to_peer(begin_msg.dump(), controller.get());

        // 3. 目录批量
        if (!dirs.empty()) {
            std::vector<std::string> dir_list(dirs.begin(), dirs.end());
            nlohmann::json msg;
            msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_BATCH;
            msg[Protocol::MSG_PAYLOAD]["creates"] = dir_list;
            msg[Protocol::MSG_PAYLOAD]["deletes"] = nlohmann::json::array();
            m_send_to_peer(msg.dump(), controller.get());
            g_logger->debug("[Broadcast] 批量发送 {} 个目录信息", dir_list.size());
        }

        // 4. 文件批量（异步背压）
        if (!files.empty()) {
            pace_and_send_file_batches(controller, session_id, std::move(files), 0);
        } else {
            g_logger->info("[Broadcast] 向 {} 批量推送完成 (0 个文件, {} 个目录, session: {})",
                           peer_id, dirs.size(), session_id);
        }
    });
}

void BroadcastManager::pace_and_send_file_batches(
    std::shared_ptr<PeerController> controller,
    uint64_t session_id,
    std::vector<FileInfo> files,
    size_t batch_index) {
    size_t total_batches = (files.size() + FILE_UPDATE_BATCH_SIZE - 1) / FILE_UPDATE_BATCH_SIZE;

    if (batch_index >= total_batches) {
        g_logger->info("[Broadcast] 向 {} 批量推送文件状态完成 ({} 个文件, session: {})",
                       controller->get_peer_id(), files.size(), session_id);
        return;
    }

    if (!controller->is_valid()) {
        g_logger->warn("[Broadcast] 连接已断开，停止发送文件 (session: {}, 已发送 {}/{})",
                       session_id, batch_index * FILE_UPDATE_BATCH_SIZE, files.size());
        return;
    }

    if (controller->get_sync_session_id() != session_id) {
        g_logger->info("[Broadcast] 会话 ID 已变更，停止本次同步 (已发送 {}/{})",
                       batch_index * FILE_UPDATE_BATCH_SIZE, files.size());
        return;
    }

    // KCP drain 回调驱动：等待发送队列有余量后再发送
    controller->on_send_ready(
        [this, controller, session_id, files = std::move(files), batch_index, total_batches]() {
            if (!controller->is_valid()) return;
            if (controller->get_sync_session_id() != session_id) return;

            size_t start = batch_index * FILE_UPDATE_BATCH_SIZE;
            size_t end = std::min(start + FILE_UPDATE_BATCH_SIZE, files.size());

            nlohmann::json msg;
            msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE_BATCH;
            msg[Protocol::MSG_PAYLOAD]["files"] = nlohmann::json::array();
            for (size_t j = start; j < end; ++j) {
                msg[Protocol::MSG_PAYLOAD]["files"].push_back(files[j]);
            }
            m_send_to_peer(msg.dump(), controller.get());

            g_logger->debug("[Broadcast] 发送文件批次 {}/{} ({} 个文件)",
                            batch_index + 1, total_batches, end - start);

            pace_and_send_file_batches(controller, session_id, std::move(files), batch_index + 1);
        });
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
            perform_flood_sync(controller, session_id);
            g_logger->info("[Anti-Entropy] 对账 flood sync 完成 (peer={}, session={})", peer_id, session_id);
        });
    }
}

}  // namespace VeritasSync
