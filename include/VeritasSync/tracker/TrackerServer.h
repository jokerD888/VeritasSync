#pragma once

#include <boost/asio.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "VeritasSync/tracker/Session.h"

using boost::asio::ip::tcp;
using nlohmann::json;

/**
 * @brief Tracker 服务器，负责接受 TCP 连接和管理会话
 * 
 * 功能：
 *   - 按 sync_key 分组管理 Session（同步房间）
 *   - 处理 REGISTER / PEER_JOIN / PEER_LEAVE 生命周期
 *   - 转发 SIGNAL 信令消息到目标 Session
 */
class TrackerServer {
public:
    TrackerServer(boost::asio::io_context& io_context, short port);

    /// 将 Session 加入指定的同步房间，并广播 PEER_JOIN
    void join(std::shared_ptr<Session> session, const std::string& sync_key);

    /// 将 Session 从同步房间移除，并广播 PEER_LEAVE
    void leave(std::shared_ptr<Session> session);

    /// 将消息转发给指定 peer_id 对应的 Session
    void forward(const std::string& to_peer_id, const json& msg);

private:
    void start_accept();

    tcp::acceptor m_acceptor;

    // sync_key -> 同组的 Session 集合
    std::map<std::string, std::set<std::shared_ptr<Session>>> m_peer_groups;
    // peer_id -> Session（快速查找）
    std::map<std::string, std::shared_ptr<Session>> m_peers_by_id;
    std::mutex m_mutex;
};
