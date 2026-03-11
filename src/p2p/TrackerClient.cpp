#include "VeritasSync/p2p/TrackerClient.h"

#include <boost/asio/detail/socket_ops.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/P2PManager.h"

namespace VeritasSync {

#ifdef _WIN32
#include <windows.h>
// 辅助函数：将系统错误码转换为 UTF-8 字符串
/**
 * @brief 将 boost::system::error_code 转换为 UTF-8 编码的错误消息字符串
 * 主要解决 Windows 下系统错误信息是 GBK 导致日志乱码的问题
 */
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
/**
 * @brief 构造函数：初始化成员变量并注册信令消息处理器
 */
TrackerClient::TrackerClient(std::string host, unsigned short port)
    : m_io_context(),
      m_work_guard(boost::asio::make_work_guard(m_io_context)),
      m_resolver(m_io_context),
      m_socket(m_io_context),
      m_retry_timer(m_io_context),
      m_host(std::move(host)),
      m_port(port) {
    register_handlers();
}

// 
/**
 * @brief 计划重连逻辑：在连接断开时清理资源并调度重连定时器
 */
void TrackerClient::schedule_reconnect() {
    m_state = State::DISCONNECTED;
    // 立即关闭，确保 socket 状态重置
    if (m_socket.is_open()) {
        boost::system::error_code ignored_ec;
        m_socket.close(ignored_ec);
    }
    
    // 清理写队列，防止重连后发送旧包
    m_write_queue.clear();

    g_logger->warn("[TrackerClient] 连接断开，{}秒后尝试重连...", RECONNECT_INTERVAL.count());

    m_retry_timer.expires_after(RECONNECT_INTERVAL);
    m_retry_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->do_connect();
        }
    });
}

/**
 * @brief 析构函数：确保在对象销毁前停止所有异步操作和后台线程
 */
TrackerClient::~TrackerClient() {
    stop();
}

/**
 * @brief 停止客户端：取消定时器、关闭 Socket、停止 io_context 并阻塞等待后台线程退出
 */
void TrackerClient::stop() {
    m_state = State::DISCONNECTED;
    
    // 取消定时器
    m_retry_timer.cancel();
    
    // 关闭 Socket，这会中断所有挂起的读写操作
    boost::system::error_code ec;
    if (m_socket.is_open()) {
        m_socket.shutdown(tcp::socket::shutdown_both, ec);
        m_socket.close(ec);
    }

    // 停止 IO 上下文
    m_io_context.stop();
    
    // 等待线程退出
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

/**
 * @brief 设置 P2PManager 引用，用于在收到 Tracker 信令时分发给 P2P 引擎
 */
void TrackerClient::set_p2p_manager(P2PManager* p2p) { m_p2p_manager = p2p; }

/**
 * @brief 设置设备 ID，用于 Tracker 注册时识别设备身份
 */
void TrackerClient::set_device_id(const std::string& device_id) { m_device_id = device_id; }

/**
 * @brief 启动连接流程：建立后台线程（如果尚未启动）并开始异步连接 Tracker
 * @param sync_key 同步密钥
 * @param on_ready 注册成功后的回调，参数为当前已在线的对等点列表
 */
void TrackerClient::connect(const std::string& sync_key, std::function<void(std::vector<std::string>)> on_ready) {
    if (m_state != State::DISCONNECTED) {
        g_logger->warn("[TrackerClient] 已经处于连接中或已连接状态。");
        return;
    }

    if (!m_p2p_manager) {
        g_logger->error("[TrackerClient] P2PManager 未设置。无法连接。");
        return;
    }
    m_sync_key = sync_key;
    m_on_ready_callback = std::move(on_ready);

    // --- 启动自己的线程 ---
    if (!m_thread.joinable()) { 
        m_thread = std::jthread([this]() {
            g_logger->info("[TrackerClient] IO context 在后台线程运行...");
            m_io_context.run();
            g_logger->info("[TrackerClient] IO context 已停止。");
        });
    }

    // 将 do_connect 任务 post 到自己的 io_context 线程
    boost::asio::post(m_io_context, [self = shared_from_this()]() { self->do_connect(); });
}

/**
 * @brief 执行异步连接：首先进行域名解析，然后发起 TCP 连接
 */
void TrackerClient::do_connect() {
    m_state = State::CONNECTING;
    // 使用异步解析，回调签名更新为 (error_code, results_type)
    m_resolver.async_resolve(
        m_host, std::to_string(m_port),
        [self = shared_from_this()](const boost::system::error_code& ec, tcp::resolver::results_type results) {
            if (ec) {
                g_logger->error("[TrackerClient] DNS 解析失败 ({}): {}", self->m_host, sys_err_to_utf8(ec));
                self->schedule_reconnect();
                return;
            }

            // async_connect 可以直接接受 results_type 集合
            boost::asio::async_connect(
                self->m_socket, results, [self](const boost::system::error_code& ec, const tcp::endpoint& endpoint) {
                    if (ec) {
                        g_logger->error("[TrackerClient] 连接 Tracker 失败: {}", sys_err_to_utf8(ec));
                        self->schedule_reconnect();
                        return;
                    }
                    g_logger->info("[TrackerClient] 已连接到 Tracker {}:{} ({})", self->m_host, self->m_port,
                                   endpoint.address().to_string());
                    self->do_register();
                    self->do_read_header();
                });
        });
}
/**
 * @brief 向 Tracker 发送注册消息，携带同步密钥和设备 ID 以加入相应的同步网络
 */
void TrackerClient::do_register() {
    nlohmann::json payload;
    payload["sync_key"] = m_sync_key;
    
    // 【关键】发送配置文件中的设备 ID，而非依赖 Tracker 分配 ip:port
    if (!m_device_id.empty()) {
        payload["device_id"] = m_device_id;
        g_logger->debug("[TrackerClient] 使用设备 ID: {}", m_device_id);
    }
    
    nlohmann::json msg;
    msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_REGISTER;
    msg[SignalProto::MSG_PAYLOAD] = payload;
    do_write(msg.dump());
}

/**
 * @brief 发送 P2P 信令消息（如 ICE Candidate, Offer/Answer）到指定的对等点
 */
void TrackerClient::send_signaling_message(const std::string& to_peer_id, const std::string& type,
                                           const std::string& sdp) {
    nlohmann::json payload;
    payload["from"] = m_self_id;
    payload["to"] = to_peer_id;
    payload["signal_type"] = type;
    payload["sdp"] = sdp;
    nlohmann::json msg;
    msg[SignalProto::MSG_TYPE] = SignalProto::TYPE_SIGNAL;
    msg[SignalProto::MSG_PAYLOAD] = payload;

    std::string msg_str = msg.dump();
    boost::asio::post(m_io_context, [self = shared_from_this(), msg_str = std::move(msg_str)]() {
        self->do_write(msg_str);
    });
}
/**
 * @brief 读取消息头：异步读取 4 字节的长度前缀（大端序）
 */
void TrackerClient::do_read_header() {
    // 读取消息头固定长度4字节。注意：不调用 consume(size())，让 asio Streambuf 自动管理读取位置
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(4),
        [self = shared_from_this()](const boost::system::error_code& ec, std::size_t /*bytes*/) {
            if (ec) {
                if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted) {
                    g_logger->error("[TrackerClient] 读取头部失败: {}", sys_err_to_utf8(ec));
                } else if (ec == boost::asio::error::eof) {
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
            if (msg_len > MAX_PACKET_SIZE) {
                g_logger->error("[TrackerClient] 消息体过长 ({} bytes)。断开连接。", msg_len);
                self->schedule_reconnect();
                return;
            }
            self->do_read_body(msg_len);
        });
}

/**
 * @brief 读取消息体：根据头部指定的长度异步读取 JSON 数据包内容
 */
void TrackerClient::do_read_body(unsigned int msg_len) {
    boost::asio::async_read(m_socket, m_read_buffer, boost::asio::transfer_exactly(msg_len),
        [self = shared_from_this()](const boost::system::error_code& ec, std::size_t /*bytes*/) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    g_logger->error("[TrackerClient] 读取消息体失败: {}", sys_err_to_utf8(ec));
                }
                self->schedule_reconnect();
                return;
            }
            
            try {
                // 性能优化：直接从 istream 解析 JSON，避免中间字符串拷贝
                std::istream is(&self->m_read_buffer);
                self->handle_message(nlohmann::json::parse(is));
            } catch (const std::exception& e) {
                g_logger->error("[TrackerClient] 解析 JSON 失败: {}", e.what());
            }
            self->do_read_header();
        });
}
/**
 * @brief 分发消息：从 JSON 数据包中提取消息类型，并调用对应的处理器
 */
void TrackerClient::handle_message(const nlohmann::json& msg) {
    try {
        const std::string type = msg.at(SignalProto::MSG_TYPE).get<std::string>();
        auto it = m_handlers.find(type);
        if (it != m_handlers.end()) {
            it->second(msg.at(SignalProto::MSG_PAYLOAD));
        } else {
            g_logger->warn("[TrackerClient] 收到未知消息类型: {}", type);
        }
    } catch (const std::exception& e) {
        g_logger->error("[TrackerClient] 分发消息失败: {}", e.what());
    }
}

/**
 * @brief 注册处理器：建立协议消息类型字符串与逻辑回调函数之间的映射
 */
void TrackerClient::register_handlers() {
    m_handlers[SignalProto::TYPE_REG_ACK] = [this](const nlohmann::json& payload) {
        m_state = State::CONNECTED;
        m_self_id = payload.at("self_id").get<std::string>();
        std::vector<std::string> peers = payload.at("peers").get<std::vector<std::string>>();
        g_logger->info("[TrackerClient] 注册成功。我的 ID: {}。收到 {} 个对等点。", m_self_id, peers.size());

        // 【修复】无论是首次连接还是重连，都要尝试连接到对等点
        if (m_on_ready_callback) {
            // 首次连接：使用回调
            auto cb = std::move(m_on_ready_callback);
            m_on_ready_callback = nullptr;
            boost::asio::post(m_p2p_manager->get_io_context(),
                              [cb, peers = std::move(peers)]() { cb(peers); });
        } else if (m_p2p_manager && !peers.empty()) {
            // 重连后：直接调用，使用 force=true 强制重建连接
            // 因为 Tracker 重连意味着网络可能已变化，旧的 ICE 连接无效
            g_logger->info("[TrackerClient] Tracker 重连成功，强制重新连接到 {} 个对等点", peers.size());
            boost::asio::post(m_p2p_manager->get_io_context(),
                              [p2p = m_p2p_manager, peers = std::move(peers)]() { 
                                  p2p->connect_to_peers(peers, true);  // force=true
                              });
        }
    };

    m_handlers[SignalProto::TYPE_PEER_JOIN] = [this](const nlohmann::json& payload) {
        std::string peer_id = payload.at("peer_id").get<std::string>();
        g_logger->info("[TrackerClient] 对等点 {} 已加入。", peer_id);
        if (m_p2p_manager) {
            // 【修复】使用 force=true，因为对端重新上线后 IP 可能已变化
            // 这也会清除之前的重连计数，给连接一个干净的开始
            boost::asio::post(m_p2p_manager->get_io_context(),
                              [p2p = m_p2p_manager, id = std::move(peer_id)]() { 
                                  p2p->connect_to_peers({id}, true);  // force=true
                              });
        }
    };

    m_handlers[SignalProto::TYPE_PEER_LEAVE] = [this](const nlohmann::json& payload) {
        std::string peer_id = payload.at("peer_id").get<std::string>();
        g_logger->info("[TrackerClient] 对等点 {} 已离开。", peer_id);
        if (m_p2p_manager) {
            boost::asio::post(m_p2p_manager->get_io_context(),
                              [p2p = m_p2p_manager, id = std::move(peer_id)]() { p2p->handle_peer_leave(id); });
        }
    };

    m_handlers[SignalProto::TYPE_SIGNAL] = [this](const nlohmann::json& payload) {
        std::string from = payload.at("from").get<std::string>();
        std::string signal_type = payload.at("signal_type").get<std::string>();
        std::string sdp = payload.at("sdp").get<std::string>();
        if (m_p2p_manager) {
            boost::asio::post(m_p2p_manager->get_io_context(),
                              [p2p = m_p2p_manager, f = std::move(from), t = std::move(signal_type), s = std::move(sdp)]() {
                                  p2p->handle_signaling_message(f, t, s);
                              });
        }
    };
}

/**
 * @brief 序列化并写入消息：添加 4 字节大端序长度头，并放入异步发送队列
 */
void TrackerClient::do_write(const std::string& msg) {
    uint32_t len_net = boost::asio::detail::socket_ops::host_to_network_long(static_cast<uint32_t>(msg.length()));
    std::string packet;
    packet.resize(4 + msg.length());
    std::memcpy(&packet[0], &len_net, 4);
    std::memcpy(&packet[4], msg.data(), msg.length());

    bool write_in_progress = !m_write_queue.empty();
    m_write_queue.push_back(std::move(packet));
    if (!write_in_progress) {
        start_write_next();
    }
}

/**
 * @brief 启动下一个写入任务：确信异步写入操作是串行执行的，防止数据混乱
 */
void TrackerClient::start_write_next() {
    if (m_write_queue.empty()) return;

    boost::asio::async_write(m_socket, boost::asio::buffer(m_write_queue.front()),
                             [self = shared_from_this()](const boost::system::error_code& ec, std::size_t) {
                                 if (ec) {
                                     if (ec != boost::asio::error::operation_aborted) {
                                         g_logger->error("[TrackerClient] 写入失败: {}", sys_err_to_utf8(ec));
                                         self->schedule_reconnect();
                                     }
                                     return;
                                 }

                                 self->m_write_queue.pop_front();
                                 if (!self->m_write_queue.empty()) {
                                     self->start_write_next();
                                 }
                             });
}
/**
 * @brief 安全关闭 Socket：将关闭操作 Post 到异步线程以避免竞态
 */
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
