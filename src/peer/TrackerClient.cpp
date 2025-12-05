#include "VeritasSync/TrackerClient.h"

#include <boost/asio/detail/socket_ops.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "VeritasSync/EncodingUtils.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"

namespace VeritasSync {

#ifdef _WIN32
#include <windows.h>
// 辅助函数：将系统错误码转换为 UTF-8 字符串
std::string sys_err_to_utf8(const boost::system::error_code& ec) {
    // 获取系统默认语言的错误消息 (通常是 GBK)
    LPWSTR messageBuffer = nullptr;
    size_t size =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, ec.value(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);

    if (size == 0 || messageBuffer == nullptr) {
        return ec.message();  // 回退到默认
    }

    std::wstring wmsg(messageBuffer, size);
    LocalFree(messageBuffer);

    // 转换为 UTF-8
    std::string utf8_msg = WideToUtf8(wmsg);

    // 移除末尾可能存在的换行符
    while (!utf8_msg.empty() && (utf8_msg.back() == '\r' || utf8_msg.back() == '\n')) {
        utf8_msg.pop_back();
    }

    return utf8_msg;
}
#else
// 非 Windows 平台直接返回原消息
std::string sys_err_to_utf8(const boost::system::error_code& ec) { return ec.message(); }
#endif

// --- 构造函数 ---
TrackerClient::TrackerClient(std::string host, unsigned short port)
    : m_io_context(),                                            // 初始化自己的 io_context
      m_work_guard(boost::asio::make_work_guard(m_io_context)),  // 保持 io_context 运行
      m_socket(m_io_context),
      m_retry_timer(m_io_context),  // socket 使用自己的 io_context
      m_host(std::move(host)),
      m_port(port) {}

void TrackerClient::schedule_reconnect() {
    m_connected = false;
    // 立即关闭，确保 socket 状态重置
    if (m_socket.is_open()) {
        boost::system::error_code ignored_ec;
        m_socket.close(ignored_ec);
    }

    g_logger->warn("[TrackerClient] 连接断开，5秒后尝试重连...");

    m_retry_timer.expires_after(std::chrono::seconds(5));
    //  捕获 self = shared_from_this() 而不是 this
    m_retry_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {  // ec == operation_aborted 说明定时器被取消了
            self->do_connect();
        }
    });
}

TrackerClient::~TrackerClient() {
    // 请求 io_context 停止，并等待线程结束
    m_io_context.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void TrackerClient::set_p2p_manager(P2PManager* p2p) { m_p2p_manager = p2p; }

// --- connect 签名和实现 ---
void TrackerClient::connect(const std::string& sync_key, std::function<void(std::vector<std::string>)> on_ready) {
    if (!m_p2p_manager) {
        g_logger->error("[TrackerClient] P2PManager 未设置。无法连接。");
        return;
    }
    m_sync_key = sync_key;
    m_on_ready_callback = std::move(on_ready);

    // --- 启动自己的线程 ---
    m_thread = std::jthread([this]() {
        g_logger->info("[TrackerClient] IO context 在后台线程运行...");
        m_io_context.run();
        g_logger->info("[TrackerClient] IO context 已停止。");
    });

    // 将 do_connect 任务 post 到自己的 io_context 线程
    boost::asio::post(m_io_context, [this]() { do_connect(); });
}

void TrackerClient::do_connect() {
    tcp::resolver resolver(m_io_context);
    auto endpoints = resolver.resolve(m_host, std::to_string(m_port));
    boost::asio::async_connect(m_socket, endpoints,
                               [self = shared_from_this()](const boost::system::error_code& ec, const tcp::endpoint&) {
                                   if (ec) {
                                       g_logger->error("[TrackerClient] 连接 Tracker 失败: {}", sys_err_to_utf8(ec));
                                       self->schedule_reconnect();
                                       return;
                                   }
                                   g_logger->info("[TrackerClient] 已连接到 Tracker {}:{}", self->m_host, self->m_port);
                                   self->do_register();
                                   self->do_read_header();
                               });
}
void TrackerClient::do_register() {
    nlohmann::json payload;
    payload["sync_key"] = m_sync_key;
    nlohmann::json msg;
    msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_REGISTER;
    msg[SignalProto::MSG_PAYLOAD] = payload;
    do_write(msg.dump());
}

void TrackerClient::send_signaling_message(const std::string& to_peer_id, const std::string& type,
                                           const std::string& sdp) {
    // --- 确保 post 到自己的 io_context 线程 ---
    nlohmann::json payload;
    payload["from"] = m_self_id;
    payload["to"] = to_peer_id;
    payload["signal_type"] = type;
    payload["sdp"] = sdp;
    nlohmann::json msg;
    msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_SIGNAL;
    msg[SignalProto::MSG_PAYLOAD] = payload;

    std::string msg_str = msg.dump();
    boost::asio::post(m_io_context, [this, msg_str]() { do_write(msg_str); });
}
void TrackerClient::do_read_header() {
    m_read_buffer.consume(m_read_buffer.size());
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(4),
                            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
                                if (ec) {
                                    if (ec != boost::asio::error::eof) {
                                        g_logger->error("[TrackerClient] 读取头部失败: {}", sys_err_to_utf8(ec));
                                    } else {
                                        g_logger->warn("[TrackerClient] Tracker 已断开连接。");
                                    }
                                    self->schedule_reconnect();
                                    return;
                                }
                                std::istream is(&self->m_read_buffer);
                                uint32_t msg_len_net;
                                is.read(reinterpret_cast<char*>(&msg_len_net), 4);
                                unsigned int msg_len =
                                    boost::asio::detail::socket_ops::network_to_host_long(msg_len_net);
                                if (msg_len > 65536) {
                                    g_logger->error("[TrackerClient] 消息体过长 ({} bytes)。断开连接。", msg_len);
                                    self->schedule_reconnect();
                                    return;
                                }
                                self->do_read_body(msg_len);
                            });
}

void TrackerClient::do_read_body(unsigned int msg_len) {
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(msg_len),
                            [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
                                if (ec) {
                                    g_logger->error("[TrackerClient] 读取消息体失败: {}", ec.message());
                                    self->schedule_reconnect();
                                    return;
                                }
                                std::istream is(&self->m_read_buffer);
                                std::string body(bytes, '\0');
                                is.read(&body[0], bytes);
                                try {
                                    self->handle_message(nlohmann::json::parse(body));
                                } catch (const std::exception& e) {
                                    g_logger->error("[TrackerClient] 解析 JSON 失败: {}", e.what());
                                }
                                self->do_read_header();
                            });
}
void TrackerClient::handle_message(const nlohmann::json& msg) {
    try {
        const std::string type = msg.at(SignalProto::MSG_TYPE).get<std::string>();
        const auto& payload = msg.at(SignalProto::MSG_PAYLOAD);
        if (type == SignalProto::TYPE_REG_ACK) {
            m_connected = true;
            m_self_id = payload.at("self_id").get<std::string>();
            std::vector<std::string> peers = payload.at("peers").get<std::vector<std::string>>();
            g_logger->info("[TrackerClient] 注册成功。我的 ID: {}。收到 {} 个对等点。", m_self_id, peers.size());

            if (m_on_ready_callback) {
                // --- 在 P2PManager 的 io_context 上调用回调 ---
                boost::asio::post(m_p2p_manager->get_io_context(), [this, peers]() { m_on_ready_callback(peers); });
            }
        } else if (type == SignalProto::TYPE_PEER_JOIN) {
            std::string peer_id = payload.at("peer_id").get<std::string>();
            g_logger->info("[TrackerClient] 对等点 {} 已加入。", peer_id);
            if (m_p2p_manager) {
                // --- 在 P2PManager 的 io_context 上调用 ---
                boost::asio::post(m_p2p_manager->get_io_context(),
                                  [this, peer_id]() { m_p2p_manager->connect_to_peers({peer_id}); });
            }
        } else if (type == SignalProto::TYPE_PEER_LEAVE) {
            std::string peer_id = payload.at("peer_id").get<std::string>();
            g_logger->info("[TrackerClient] 对等点 {} 已离开。", peer_id);
            if (m_p2p_manager) {
                // --- 在 P2PManager 的 io_context 上调用 ---
                boost::asio::post(m_p2p_manager->get_io_context(),
                                  [this, peer_id]() { m_p2p_manager->handle_peer_leave(peer_id); });
            }
        } else if (type == SignalProto::TYPE_SIGNAL) {
            std::string from = payload.at("from").get<std::string>();
            std::string signal_type = payload.at("signal_type").get<std::string>();
            std::string sdp = payload.at("sdp").get<std::string>();
            if (m_p2p_manager) {
                // --- 在 P2PManager 的 io_context 上调用 ---
                boost::asio::post(m_p2p_manager->get_io_context(), [this, from, signal_type, sdp]() {
                    m_p2p_manager->handle_signaling_message(from, signal_type, sdp);
                });
            }
        }
    } catch (const std::exception& e) {
        g_logger->error("[TrackerClient] 处理消息失败: {}", e.what());
    }
}

void TrackerClient::do_write(const std::string& msg) {
    uint32_t len_net = boost::asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(msg.length()));
    m_write_buffer.resize(4 + msg.length());
    std::memcpy(m_write_buffer.data(), &len_net, 4);
    std::memcpy(m_write_buffer.data() + 4, msg.c_str(), msg.length());
    boost::asio::async_write(m_socket, boost::asio::buffer(m_write_buffer),
                             [self = shared_from_this()](const boost::system::error_code& ec, std::size_t bytes) {
                                 if (ec) {
                                     g_logger->error("[TrackerClient] 写入失败: {}", ec.message());
                                     self->schedule_reconnect();
                                 }
                             });
}
void TrackerClient::close_socket() {
    // --- post 到自己的 io_context ---
    boost::asio::post(m_io_context, [this]() {
        if (m_socket.is_open()) {
            boost::system::error_code ec;
            m_socket.shutdown(tcp::socket::shutdown_both, ec);
            m_socket.close(ec);
        }
    });
}

}  // namespace VeritasSync
