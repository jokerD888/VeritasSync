#include "VeritasSync/tracker/TrackerServer.h"
#include "VeritasSync/common/SignalProto.h"

#include <spdlog/spdlog.h>

extern std::shared_ptr<spdlog::logger> g_logger;

TrackerServer::TrackerServer(boost::asio::io_context& io_context, short port)
    : m_acceptor(io_context, tcp::endpoint(tcp::v4(), port)) {
    start_accept();
}

void TrackerServer::start_accept() {
    m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<Session>(std::move(socket), *this)->start();
        }
        start_accept();
    });
}

void TrackerServer::join(std::shared_ptr<Session> session, const std::string& sync_key) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& peer_set = m_peer_groups[sync_key];
    std::string new_peer_id = session->get_id();

    // 检查是否已存在相同 ID 的旧 session（重连场景）
    auto old_session_it = m_peers_by_id.find(new_peer_id);
    if (old_session_it != m_peers_by_id.end()) {
        auto old_session = old_session_it->second;
        if (old_session != session) {
            g_logger->info("[Tracker] 检测到相同 ID 的旧连接，移除旧 session: {}", new_peer_id);
            peer_set.erase(old_session);
            m_peers_by_id.erase(old_session_it);
        }
    }

    // 1. 准备 ACK 消息，包含当前房间中的所有 peers（排除自己）
    json reg_ack_payload;
    reg_ack_payload["self_id"] = new_peer_id;

    json peers_list = json::array();
    for (const auto& peer : peer_set) {
        if (peer->get_id() != new_peer_id) {
            peers_list.push_back(peer->get_id());
        }
    }
    reg_ack_payload["peers"] = peers_list;

    json reg_ack_msg;
    reg_ack_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_REG_ACK;
    reg_ack_msg[SignalProto::MSG_PAYLOAD] = reg_ack_payload;

    session->send(reg_ack_msg);

    // 2. 准备 PEER_JOIN 广播消息
    json peer_join_payload;
    peer_join_payload["peer_id"] = new_peer_id;
    json peer_join_msg;
    peer_join_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_PEER_JOIN;
    peer_join_msg[SignalProto::MSG_PAYLOAD] = peer_join_payload;

    // 广播 PEER_JOIN 给房间中所有其他 peers
    for (const auto& peer : peer_set) {
        if (peer->get_id() != new_peer_id) {
            peer->send(peer_join_msg);
        }
    }

    // 3. 将新 peer 加入
    peer_set.insert(session);
    m_peers_by_id[new_peer_id] = session;

    g_logger->info("[Tracker] {} 已加入组 '{}'", new_peer_id, sync_key);
}

void TrackerServer::leave(std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string peer_id = session->get_id();
    std::string sync_key = session->get_sync_key();
    if (sync_key.empty()) return;  // 从未注册

    // 1. 从 m_peers_by_id 移除
    m_peers_by_id.erase(peer_id);

    // 2. 从 m_peer_groups 移除
    auto it = m_peer_groups.find(sync_key);
    if (it != m_peer_groups.end()) {
        auto& peer_set = it->second;
        peer_set.erase(session);

        // 3. 广播 PEER_LEAVE 给房间中所有剩余 peers
        json peer_leave_payload;
        peer_leave_payload["peer_id"] = peer_id;
        json peer_leave_msg;
        peer_leave_msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_PEER_LEAVE;
        peer_leave_msg[SignalProto::MSG_PAYLOAD] = peer_leave_payload;

        for (const auto& peer : peer_set) {
            peer->send(peer_leave_msg);
        }
        g_logger->info("[Tracker] {} 已离开组 '{}'", peer_id, sync_key);

        if (peer_set.empty()) {
            m_peer_groups.erase(it);
            g_logger->info("[Tracker] 组 '{}' 已清空。", sync_key);
        }
    }
}

void TrackerServer::forward(const std::string& to_peer_id, const json& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_peers_by_id.find(to_peer_id);
    if (it != m_peers_by_id.end()) {
        it->second->send(msg);
    } else {
        g_logger->warn("[Tracker] 无法转发：未找到 peer {}", to_peer_id);
    }
}
