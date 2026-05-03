#include "VeritasSync/sync/SyncSession.h"

#include <boost/asio/post.hpp>
#include <chrono>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

// E-1: 魔数统一为命名常量（保留为默认值参考，实际运行时使用成员变量）
// SYNC_TIMEOUT_SECONDS 已统一定义在 Protocol.h (Protocol::SYNC_TIMEOUT_SECONDS)
// FLOW_CONTROL_THRESHOLD 和 FLOW_CONTROL_SLEEP_MS 现在通过构造函数注入

SyncSession::SyncSession(StateManager* state_manager,
                         boost::asio::thread_pool& worker_pool,
                         boost::asio::io_context& io_context,
                         SendToPeerFunc send_to_peer,
                         WithPeerFunc with_peer,
                         GetPeerFunc get_peer,
                         ResyncCallback resync_fn,
                         int sync_timeout_seconds)
    : m_state_manager(state_manager),
      m_worker_pool(worker_pool),
      m_io_context(io_context),
      m_send_to_peer(std::move(send_to_peer)),
      m_with_peer(std::move(with_peer)),
      m_get_peer(std::move(get_peer)),
      m_resync_fn(std::move(resync_fn)),
      m_sync_timeout_seconds(sync_timeout_seconds) {}

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

        // 【冲突仲裁】双向 flood sync 时，两端的 sync_begin 会互相覆盖 session_id。
        // 解决方案：offer_side（self_id < peer_id）优先执行，answer_side 让步。
        // 让步方直接清除自己的 session_id，接受对方的 sync_begin，不做延迟重试。
        uint64_t my_session = from_peer->get_sync_session_id();
        if (my_session != 0 && from_peer->is_offer_side()) {
            g_logger->info("[Sync] 双向同步冲突：我方是 offer_side (session={})，"
                           "忽略对方的 sync_begin (session={})", my_session, session_id);
            // 对方的 sync_begin 被丢弃，我方继续自己的 flood sync。
            // 我方的 session_id 保持不变，对方稍后会收到我方的 sync_begin。
            return;
        }

        from_peer->set_sync_session_id(session_id);
        from_peer->set_expected_file_count(file_count);
        from_peer->set_expected_dir_count(dir_count);
        from_peer->reset_received_file_count();
        from_peer->reset_received_dir_count();
        
        // A-6: 通过封装方法设置超时定时器（内部加锁，消除数据竞争）
        std::string peer_id = from_peer->get_peer_id();
        from_peer->start_sync_timeout(m_sync_timeout_seconds,
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
        // 生成一个新的 ID 并通过回调请求重新同步
        uint64_t new_session_id = std::chrono::steady_clock::now().time_since_epoch().count();
        from_peer->set_sync_session_id(new_session_id);

        auto controller = m_get_peer(from_peer->get_peer_id());
        if (controller && m_resync_fn) {
            boost::asio::post(m_worker_pool, [this, controller, new_session_id]() {
                m_resync_fn(controller, new_session_id);
            });
        }
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 解析 sync_ack 失败: {}", e.what());
    }
}

}  // namespace VeritasSync
