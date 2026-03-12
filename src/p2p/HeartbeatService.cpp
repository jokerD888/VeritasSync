#include "VeritasSync/p2p/HeartbeatService.h"

#include <nlohmann/json.hpp>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

HeartbeatService::HeartbeatService(boost::asio::io_context& io_context,
                                   CollectPeersFunc collect_peers,
                                   SendToPeerFunc send_to_peer,
                                   int interval_ms)
    : m_io_context(io_context),
      m_timer(io_context),
      m_collect_peers(std::move(collect_peers)),
      m_send_to_peer(std::move(send_to_peer)),
      m_interval_ms(interval_ms) {
}

void HeartbeatService::start() {
    m_running = true;
    schedule_heartbeat();
}

void HeartbeatService::stop() {
    m_running = false;
    m_timer.cancel();
}

void HeartbeatService::schedule_heartbeat() {
    if (!m_running) return;

    m_timer.expires_after(std::chrono::milliseconds(m_interval_ms));
    m_timer.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && m_running) {
            send_heartbeats();
            schedule_heartbeat();  // 重新调度
        }
    });
}

void HeartbeatService::send_heartbeats() {
    // 只在有连接的 peer 时发送心跳
    auto connected_peers = m_collect_peers();

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

void HeartbeatService::handle_heartbeat(PeerController* from_peer) {
    if (!from_peer) return;

    // 收到心跳请求，立即回复 ACK
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_HEARTBEAT_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()}
    };

    m_send_to_peer(msg.dump(), from_peer);

    g_logger->debug("[Heartbeat] 收到来自 {} 的心跳，已回复 ACK", from_peer->get_peer_id());
}

void HeartbeatService::handle_heartbeat_ack(PeerController* from_peer) {
    if (!from_peer) return;

    // 收到心跳响应，连接正常
    // 这里可以用于统计 RTT 或检测连接质量，目前只记录日志
    g_logger->debug("[Heartbeat] 收到来自 {} 的心跳 ACK", from_peer->get_peer_id());
}

}  // namespace VeritasSync
