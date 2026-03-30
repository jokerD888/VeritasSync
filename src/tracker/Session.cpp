#include "VeritasSync/tracker/Session.h"
#include "VeritasSync/tracker/TrackerServer.h"
#include "VeritasSync/common/Config.h"
#include "VeritasSync/common/SignalProto.h"




#include <boost/asio/detail/socket_ops.hpp>
#include <cstring>
#include <spdlog/spdlog.h>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

extern std::shared_ptr<spdlog::logger> g_logger;

// E-1: 魔数统一为命名常量
static constexpr unsigned int MAX_MESSAGE_LENGTH = 262144;  // 消息体最大长度 256KB（中继数据需要更大空间）

// ═══════════════════════════════════════════════════════════════
// Windows UTF-8 辅助函数（消除 handle_read_header / handle_read_body 中的重复）
// ═══════════════════════════════════════════════════════════════

#if defined(_WIN32)
static std::string sys_err_to_utf8(const boost::system::error_code& ec) {
    LPWSTR messageBuffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, ec.value(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&messageBuffer, 0, NULL);

    if (size == 0 || messageBuffer == nullptr) {
        return ec.message();
    }

    std::wstring wmsg(messageBuffer, size);
    LocalFree(messageBuffer);

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wmsg.data(), (int)wmsg.size(), NULL, 0, NULL, NULL);
    std::string utf8_msg(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wmsg.data(), (int)wmsg.size(), &utf8_msg[0], size_needed, NULL, NULL);

    // 移除末尾换行符
    while (!utf8_msg.empty() && (utf8_msg.back() == '\r' || utf8_msg.back() == '\n')) {
        utf8_msg.pop_back();
    }
    return utf8_msg;
}
#else
static std::string sys_err_to_utf8(const boost::system::error_code& ec) { return ec.message(); }
#endif

// ═══════════════════════════════════════════════════════════════
// Session 实现
// ═══════════════════════════════════════════════════════════════

Session::Session(tcp::socket socket, TrackerServer& server)
    : m_socket(std::move(socket)),
      m_server(server) {
    std::stringstream ss;
    ss << m_socket.remote_endpoint().address().to_string()
       << ":" << m_socket.remote_endpoint().port();
    m_connection_info = ss.str();
    g_logger->info("[Session] 新连接来自: {}", m_connection_info);
}

Session::~Session() {
    g_logger->info("[Session] {} 已断开连接。", m_id);
}

void Session::start() { do_read_header(); }

void Session::send(const json& msg) {
    std::string s = msg.dump();

    uint32_t len_net = boost::asio::detail::socket_ops::host_to_network_long(
        static_cast<uint32_t>(s.length()));

    auto write_buf = std::make_shared<std::vector<char>>(4 + s.length());
    std::memcpy(write_buf->data(), &len_net, 4);
    std::memcpy(write_buf->data() + 4, s.c_str(), s.length());

    boost::asio::async_write(
        m_socket, boost::asio::buffer(*write_buf),
        [self = shared_from_this(), write_buf](const boost::system::error_code& ec, std::size_t) {
            if (ec) {
                g_logger->error("[Session] {} 写入失败: {}", self->m_id, sys_err_to_utf8(ec));
            }
        });
}

void Session::do_read_header() {
    m_read_buffer.consume(m_read_buffer.size());
    boost::asio::async_read(
        m_socket, m_read_buffer, boost::asio::transfer_exactly(4),
        [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
            self->handle_read_header(ec, bytes);
        });
}

void Session::handle_read_header(const boost::system::error_code& ec, std::size_t /*bytes*/) {
    if (ec) {
        if (ec != boost::asio::error::eof) {
            g_logger->error("[Session] {} 读取头部失败: {}", m_id, sys_err_to_utf8(ec));
        } else {
            g_logger->warn("[Session] {} 触发 EOF。", m_id);
        }
        m_server.leave(shared_from_this());
        return;
    }

    std::istream is(&m_read_buffer);
    uint32_t msg_len_net;
    is.read(reinterpret_cast<char*>(&msg_len_net), 4);
    unsigned int msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len_net);

    if (msg_len > MAX_MESSAGE_LENGTH) {
        g_logger->error("[Session] {} 消息体过长 ({} bytes)。断开连接。", m_id, msg_len);
        m_server.leave(shared_from_this());
        return;
    }

    do_read_body(msg_len);
}

void Session::do_read_body(unsigned int msg_len) {
    boost::asio::async_read(
        m_socket, m_read_buffer, boost::asio::transfer_exactly(msg_len),
        [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
            self->handle_read_body(ec, bytes);
        });
}

void Session::handle_read_body(const boost::system::error_code& ec, std::size_t bytes) {
    if (ec) {
        g_logger->error("[Session] {} 读取消息体失败: {}", m_id, sys_err_to_utf8(ec));
        m_server.leave(shared_from_this());
        return;
    }

    std::istream is(&m_read_buffer);
    std::string body(bytes, '\0');
    is.read(&body[0], bytes);

    try {
        json msg = json::parse(body);
        handle_message(msg);
    } catch (const std::exception& e) {
        g_logger->error("[Session] {} 解析 JSON 失败: {}", m_id, e.what());
    }

    do_read_header();
}

void Session::handle_message(const json& msg) {
    try {
        const std::string type = msg.at(SignalProto::MSG_TYPE).get<std::string>();
        const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);

        if (type == SignalProto::TYPE_REGISTER) {
            handle_register(payload);
        } else if (type == SignalProto::TYPE_SIGNAL) {
            handle_signal(msg);
        } else if (type == SignalProto::TYPE_RELAY_DATA) {
            handle_relay(msg);
        }
    } catch (const std::exception& e) {
        g_logger->error("[Session] {} 处理消息失败: {}", m_id, e.what());
    }
}

void Session::handle_register(const json& payload) {
    m_sync_key = payload.at("sync_key").get<std::string>();
    const std::string sync_key_error = VeritasSync::get_sync_key_validation_error(m_sync_key);
    if (!sync_key_error.empty()) {
        g_logger->error("[Session] {} 注册失败：{}。", m_connection_info, sync_key_error);
        return;
    }

    if (!payload.contains("device_id") || payload.at("device_id").get<std::string>().empty()) {

        g_logger->error("[Session] {} 注册失败：device_id 为空或缺失。", m_connection_info);
        return;
    }

    m_id = payload.at("device_id").get<std::string>();
    g_logger->info("[Session] {} 注册成功，device_id: {}", m_connection_info, m_id);

    m_server.join(shared_from_this(), m_sync_key);
}

void Session::handle_signal(const json& msg) {
    const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);
    std::string to_peer_id = payload.at("to").get<std::string>();

    if (to_peer_id.empty()) {
        g_logger->error("[Session] {} 转发失败：'to' 字段为空。", m_id);
        return;
    }

    m_server.forward(to_peer_id, msg);
}

void Session::handle_relay(const json& msg) {
    const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);
    std::string to_peer_id = payload.at("to").get<std::string>();

    if (to_peer_id.empty()) {
        g_logger->error("[Session] {} 中继转发失败：'to' 字段为空。", m_id);
        return;
    }

    // 【安全】用服务端验证后的 session ID 覆盖 from 字段，防止伪造
    json relay_msg = msg;
    relay_msg[SignalProto::MSG_PAYLOAD]["from"] = m_id;

    m_server.forward(to_peer_id, relay_msg);
}
