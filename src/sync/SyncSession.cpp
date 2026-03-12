#include "VeritasSync/sync/SyncSession.h"

#include <boost/asio/post.hpp>
#include <chrono>
#include <set>
#include <thread>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

// E-1: 魔数统一为命名常量
static constexpr int SYNC_TIMEOUT_SECONDS          = 60;    // 同步会话超时（秒）
static constexpr int FLOW_CONTROL_THRESHOLD         = 1024;  // KCP 发送队列流控阈值
static constexpr int FLOW_CONTROL_SLEEP_MS          = 20;    // 流控等待间隔（毫秒）

SyncSession::SyncSession(StateManager* state_manager,
                         boost::asio::thread_pool& worker_pool,
                         boost::asio::io_context& io_context,
                         SendToPeerFunc send_to_peer,
                         WithPeerFunc with_peer,
                         GetPeerFunc get_peer)
    : m_state_manager(state_manager),
      m_worker_pool(worker_pool),
      m_io_context(io_context),
      m_send_to_peer(std::move(send_to_peer)),
      m_with_peer(std::move(with_peer)),
      m_get_peer(std::move(get_peer)) {}

// ═══════════════════════════════════════════════════════════════
// 同步会话管理
// ═══════════════════════════════════════════════════════════════

void SyncSession::send_sync_begin(PeerController* peer, uint64_t session_id, 
                                   size_t file_count, size_t dir_count) {
    if (!peer) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_BEGIN;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"file_count", file_count},
        {"dir_count", dir_count}
    };
    m_send_to_peer(msg.dump(), peer);
}

void SyncSession::send_sync_ack(PeerController* peer, uint64_t session_id, 
                                 size_t received_files, size_t received_dirs) {
    if (!peer) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"received_files", received_files},
        {"received_dirs", received_dirs}
    };
    m_send_to_peer(msg.dump(), peer);
}

void SyncSession::handle_sync_begin(const nlohmann::json& payload, PeerController* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t session_id = payload.at("session_id").get<uint64_t>();
        size_t file_count = payload.at("file_count").get<size_t>();
        size_t dir_count = payload.at("dir_count").get<size_t>();
        
        g_logger->info("[Sync] 收到同步开始: session={}, files={}, dirs={}", 
                       session_id, file_count, dir_count);
        
        from_peer->set_sync_session_id(session_id);
        from_peer->set_expected_file_count(file_count);
        from_peer->set_expected_dir_count(dir_count);
        from_peer->reset_received_file_count();
        from_peer->reset_received_dir_count();
        
        // A-6: 通过封装方法设置超时定时器（内部加锁，消除数据竞争）
        std::string peer_id = from_peer->get_peer_id();
        from_peer->start_sync_timeout(SYNC_TIMEOUT_SECONDS,
            [this, peer_id, session_id](const boost::system::error_code& ec) {
                if (ec) return;  // 被取消
                
                m_with_peer(peer_id, [this, session_id](PeerController* peer) {
                    if (peer->get_sync_session_id() != session_id) return;
                    
                    size_t recv_files = peer->get_received_file_count();
                    size_t expect_files = peer->get_expected_file_count();
                    size_t recv_dirs = peer->get_received_dir_count();
                    size_t expect_dirs = peer->get_expected_dir_count();
                    
                    if (recv_files < expect_files || recv_dirs < expect_dirs) {
                        g_logger->warn("[Sync] 同步会话 {} 超时 (文件: {}/{}, 目录: {}/{})", 
                                       session_id, recv_files, expect_files, recv_dirs, expect_dirs);
                    }
                    
                    send_sync_ack(peer, session_id, recv_files, recv_dirs);
                    peer->set_sync_session_id(0);
                });
            });
            
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 处理 sync_begin 失败: {}", e.what());
    }
}

void SyncSession::handle_sync_ack(const nlohmann::json& payload, PeerController* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t ack_session_id = payload.at("session_id").get<uint64_t>();
        size_t received_files = payload.at("received_files").get<size_t>();
        size_t received_dirs = payload.at("received_dirs").get<size_t>();
        
        // 【关键】检查这是否针对当前活跃会话的回复
        uint64_t current_id = from_peer->get_sync_session_id();
        if (ack_session_id != current_id) {
            g_logger->warn("[Sync] 收到过时或不匹配的 ACK (ACK ID: {}, 当前 ID: {})，忽略。", 
                           ack_session_id, current_id);
            return;
        }

        g_logger->warn("[Sync] 收到 ACK (ID: {}): 对方只收到 {} 文件, {} 目录。重新同步...",
                       ack_session_id, received_files, received_dirs);
        
        // 既然对方没收完，且会话 ID 匹配，说明确实需要补发
        // 生成一个新的 ID 并重新开始推送
        std::string peer_id = from_peer->get_peer_id();
        uint64_t new_session_id = std::chrono::steady_clock::now().time_since_epoch().count();
        from_peer->set_sync_session_id(new_session_id);
        
        boost::asio::post(m_worker_pool, [this, peer_id, new_session_id]() {
            // 重新获取 controller 的 shared_ptr
            auto controller = m_get_peer(peer_id);
            if (controller) {
                perform_flood_sync(controller, new_session_id);
            }
        });
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 解析 sync_ack 失败: {}", e.what());
    }
}

void SyncSession::perform_flood_sync(std::shared_ptr<PeerController> controller, uint64_t session_id) {
    if (!controller || !controller->is_valid() || !m_state_manager) {
        g_logger->warn("[Sync] perform_flood_sync: 上下文无效，跳过");
        return;
    }
    
    std::string peer_id = controller->get_peer_id();
    
    // 检查 session_id 是否一致（防止重复执行旧会话）
    if (controller->get_sync_session_id() != session_id) {
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
    boost::asio::post(m_io_context, [this, controller, session_id, 
                                     file_count = files.size(), dir_count = dirs.size()]() {
        if (controller->is_valid()) {
            send_sync_begin(controller.get(), session_id, file_count, dir_count);
        }
    });

    // 【阶段1优化】3. 批量发送目录信息
    if (!dirs.empty()) {
        std::vector<std::string> dir_list(dirs.begin(), dirs.end());
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_BATCH;
        msg[Protocol::MSG_PAYLOAD]["creates"] = dir_list;
        msg[Protocol::MSG_PAYLOAD]["deletes"] = nlohmann::json::array();  // flood sync 只有创建

        std::weak_ptr<PeerController> weak_ctrl = controller;
        boost::asio::post(m_io_context, [this, weak_ctrl, msg_str = msg.dump()]() {
            auto ctrl_locked = weak_ctrl.lock();
            if (ctrl_locked && ctrl_locked->is_valid() && ctrl_locked->is_connected()) {
                m_send_to_peer(msg_str, ctrl_locked.get());
            }
        });
        
        g_logger->debug("[P2P] 批量发送 {} 个目录信息", dir_list.size());
    }

    // 【阶段1优化】4. 批量发送文件状态
    for (size_t i = 0; i < files.size(); i += FILE_UPDATE_BATCH_SIZE) {
        // 检查连接有效性和会话有效性
        if (!controller->is_valid()) {
            g_logger->warn("[P2P] 连接已断开，停止发送文件 (session: {}, 已发送 {}/{})", 
                           session_id, i, files.size());
            return;
        }
        
        if (controller->get_sync_session_id() != session_id) {
            g_logger->info("[Sync] 会话 ID 已变更，停止本次同步 (已发送 {}/{})", i, files.size());
            return;
        }
        
        size_t end = std::min(i + FILE_UPDATE_BATCH_SIZE, files.size());
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE_BATCH;
        msg[Protocol::MSG_PAYLOAD]["files"] = nlohmann::json::array();
        
        for (size_t j = i; j < end; ++j) {
            msg[Protocol::MSG_PAYLOAD]["files"].push_back(files[j]);
        }

        std::weak_ptr<PeerController> weak_ctrl = controller;
        boost::asio::post(m_io_context, [this, weak_ctrl, msg_str = msg.dump()]() {
            auto ctrl_locked = weak_ctrl.lock();
            if (ctrl_locked && ctrl_locked->is_valid() && ctrl_locked->is_connected()) {
                m_send_to_peer(msg_str, ctrl_locked.get());
            }
        });

        // 【流控】每发送一个批次检查一次 KCP 发送队列积压量
        int pending = controller->get_kcp_wait_send();
        while (pending > FLOW_CONTROL_THRESHOLD && controller->is_valid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(FLOW_CONTROL_SLEEP_MS));
            pending = controller->get_kcp_wait_send();
        }
        
        g_logger->debug("[P2P] 发送文件批次 {}/{} ({} 个文件)", 
                       (i / FILE_UPDATE_BATCH_SIZE) + 1,
                       (files.size() + FILE_UPDATE_BATCH_SIZE - 1) / FILE_UPDATE_BATCH_SIZE,
                       end - i);
    }

    g_logger->info("[P2P] 向 {} 批量推送文件状态完成 ({} 个文件, {} 个目录, session: {})", 
                   peer_id, files.size(), dirs.size(), session_id);
}

}  // namespace VeritasSync
