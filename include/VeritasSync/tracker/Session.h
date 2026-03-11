#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using boost::asio::ip::tcp;
using nlohmann::json;

class TrackerServer;  // 前向声明

/**
 * @brief TCP 会话类，处理持久连接和 JSON 消息收发
 * 
 * 每个客户端连接对应一个 Session 实例。
 * Session 负责：
 *   - 读取 4 字节长度前缀 + JSON 消息体
 *   - 解析并分发消息到相应的处理器
 *   - 异步发送 JSON 响应
 */
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, TrackerServer& server);
    ~Session();

    void start();

    std::string get_id() const { return m_id; }
    std::string get_sync_key() const { return m_sync_key; }

    /// 异步发送 JSON 消息（线程安全：每次独立分配写缓冲区）
    void send(const json& msg);

private:
    void do_read_header();
    void handle_read_header(const boost::system::error_code& ec, std::size_t bytes);
    void do_read_body(unsigned int msg_len);
    void handle_read_body(const boost::system::error_code& ec, std::size_t bytes);

    void handle_message(const json& msg);
    void handle_register(const json& payload);
    void handle_signal(const json& msg);

    tcp::socket m_socket;
    TrackerServer& m_server;
    std::string m_id;              ///< peer_id（由客户端的 device_id 设置）
    std::string m_connection_info; ///< 原始 ip:port，仅用于日志
    std::string m_sync_key;

    boost::asio::streambuf m_read_buffer;
};
