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
 * @brief 构造函数：使用外部 io_context，不再创建独立线程
 */
TrackerClient::TrackerClient(boost::asio::io_context& io_context, std::string host, unsigned short port)
    : m_io_context(io_context),
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
    // stop() 后禁止重连，避免 operation_aborted/eof 路径触发"被动复活"
    if (is_stopping()) {
        g_logger->info("[TrackerClient] 客户端处于停止态，跳过自动重连。");
        return;
    }

    m_state.store(State::DISCONNECTED, std::memory_order_release);

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
        if (!ec && !self->is_stopping()) {
            self->do_connect();
        }
    });
}

/**
 * @brief 析构函数：确保在对象销毁前停止所有异步操作和后台线程
 */
TrackerClient::~TrackerClient() {
    // 析构阶段可能已不再受 shared_ptr 管理，直接同步清理。
    stop_impl();
}

/**
 * @brief 停止客户端：取消定时器、关闭 Socket
 * 不再需要停止 io_context 或等待线程退出（共享外部 io_context）
 */
void TrackerClient::stop() {
    // 单一原子写，替代原来的两个 atomic<bool> 标志
    m_state.store(State::STOPPING, std::memory_order_release);

    // 正常生命周期下将 stop 串行化到 io_context 线程执行
    if (auto self = weak_from_this().lock()) {
        boost::asio::dispatch(m_io_context, [self]() { self->stop_impl(); });
    } else {
        // 兜底：对象已进入析构流程时，直接同步清理
        stop_impl();
    }
}

void TrackerClient::stop_impl() {
    // stop_impl 在 STOPPING 状态后执行清理，不覆盖状态

    // 清理会话回调，避免旧会话回调泄漏到下一轮 connect
    m_on_ready_callback = nullptr;
    m_pending_ready_callbacks.clear();

    // 取消定时器
    m_retry_timer.cancel();

    // 关闭 Socket，这会中断所有挂起的读写操作
    boost::system::error_code ec;
    if (m_socket.is_open()) {
        m_socket.shutdown(tcp::socket::shutdown_both, ec);
        m_socket.close(ec);
    }

    m_write_queue.clear();
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
 * @brief 启动连接流程：直接 post 到共享的 io_context 开始异步连接 Tracker
 * @param sync_key 同步密钥
 * @param on_ready 注册成功后的回调，参数为当前已在线的对等点列表
 */
void TrackerClient::connect(const std::string& sync_key, std::function<void(std::vector<std::string>)> on_ready) {
    // 将"状态检查+状态迁移+回调写入"统一到 io_context 线程，避免跨线程竞态
    boost::asio::post(m_io_context,
                      [self = shared_from_this(), sync_key, on_ready = std::move(on_ready)]() mutable {
                          if (!self->m_p2p_manager) {
                              g_logger->error("[TrackerClient] P2PManager 未设置。无法连接。");
                              return;
                          }

                          if (self->m_state.load(std::memory_order_acquire) != State::DISCONNECTED &&
                              !self->is_stopping()) {
                              // 在连接过程中允许同 sync_key 的回调合并，避免静默丢回调
                              auto cur = self->m_state.load(std::memory_order_acquire);
                              if (on_ready &&
                                  (cur == State::CONNECTING || cur == State::REGISTERING)) {
                                  if (self->m_sync_key == sync_key) {
                                      self->m_pending_ready_callbacks.push_back(std::move(on_ready));
                                      g_logger->info("[TrackerClient] 连接进行中，已合并一个 on_ready 回调。");
                                  } else {
                                      g_logger->warn("[TrackerClient] 连接进行中且 sync_key 不一致，忽略本次 connect。active={}, incoming={}",
                                                     self->m_sync_key, sync_key);
                                  }
                              } else {
                                  g_logger->warn("[TrackerClient] 已经处于连接中或已连接状态。");
                              }
                              return;
                          }

                          // 新一轮手动 connect：重置为 DISCONNECTED（解除 STOPPING 状态）
                          self->m_state.store(State::DISCONNECTED, std::memory_order_release);
                          self->m_pending_ready_callbacks.clear();
                          self->m_sync_key = sync_key;
                          self->m_on_ready_callback = std::move(on_ready);
                          self->do_connect();
                      });
}


/**
 * @brief 执行异步连接：首先进行域名解析，然后发起 TCP 连接
 */
void TrackerClient::do_connect() {
    if (is_stopping()) {
        m_state.store(State::DISCONNECTED, std::memory_order_release);
        return;
    }

    m_state.store(State::CONNECTING, std::memory_order_release);
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
    if (is_stopping()) {
        return;
    }

    m_state.store(State::REGISTERING, std::memory_order_release);
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
    // 【健壮性修复 M14】断连或停止时不发送信令，避免消息堆积
    auto state = m_state.load(std::memory_order_acquire);
    if (state != State::CONNECTED && state != State::REGISTERING) {
        if (g_logger) g_logger->debug("[TrackerClient] 跳过信令发送（未连接）: {} -> {}", type, to_peer_id);
        return;
    }

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
                if (ec == boost::asio::error::operation_aborted) {
                    // stop() 主动取消时不应触发重连
                    return;
                }
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
                if (ec == boost::asio::error::operation_aborted) {
                    // stop() 主动取消时不应触发重连
                    return;
                }
                g_logger->error("[TrackerClient] 读取消息体失败: {}", sys_err_to_utf8(ec));
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
 * 
 * 由于 TrackerClient 与 P2PManager 共享同一个 io_context，
 * 回调可以直接调用 P2PManager 的方法，无需跨线程 post。
 */
void TrackerClient::register_handlers() {
    m_handlers[SignalProto::TYPE_REG_ACK] = [this](const nlohmann::json& payload) {
        m_state.store(State::CONNECTED, std::memory_order_release);
        m_self_id = payload.at("self_id").get<std::string>();
        std::vector<std::string> peers = payload.at("peers").get<std::vector<std::string>>();
        g_logger->info("[TrackerClient] 注册成功。我的 ID: {}。收到 {} 个对等点。", m_self_id, peers.size());

        bool has_ready_callbacks = false;

        // 首次连接回调
        if (m_on_ready_callback) {
            auto cb = std::move(m_on_ready_callback);
            m_on_ready_callback = nullptr;
            has_ready_callbacks = true;
            cb(peers);
        }

        // 连接过程中合并的重复 connect 回调
        if (!m_pending_ready_callbacks.empty()) {
            auto pending = std::move(m_pending_ready_callbacks);
            m_pending_ready_callbacks.clear();
            has_ready_callbacks = true;
            for (auto& cb : pending) {
                if (cb) cb(peers);
            }
        }

        // 无回调时按重连语义处理
        if (!has_ready_callbacks && m_p2p_manager && !peers.empty()) {
            g_logger->info("[TrackerClient] Tracker 重连成功，强制重新连接到 {} 个对等点", peers.size());
            m_p2p_manager->connect_to_peers(peers, true);  // force=true
        }
    };

    m_handlers[SignalProto::TYPE_PEER_JOIN] = [this](const nlohmann::json& payload) {
        std::string peer_id = payload.at("peer_id").get<std::string>();
        g_logger->info("[TrackerClient] 对等点 {} 已加入。", peer_id);
        if (m_p2p_manager) {
            m_p2p_manager->connect_to_peers({peer_id}, true);  // force=true
        }
    };

    m_handlers[SignalProto::TYPE_PEER_LEAVE] = [this](const nlohmann::json& payload) {
        std::string peer_id = payload.at("peer_id").get<std::string>();
        g_logger->info("[TrackerClient] 对等点 {} 已离开。", peer_id);
        if (m_p2p_manager) {
            m_p2p_manager->handle_peer_leave(peer_id);
        }
    };

    m_handlers[SignalProto::TYPE_SIGNAL] = [this](const nlohmann::json& payload) {
        std::string from = payload.at("from").get<std::string>();
        std::string signal_type = payload.at("signal_type").get<std::string>();
        std::string sdp = payload.at("sdp").get<std::string>();
        if (m_p2p_manager) {
            m_p2p_manager->handle_signaling_message(from, signal_type, sdp);
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
 * @brief 安全关闭 Socket：将关闭操作 Post 到 io_context 以确保串行化
 */
void TrackerClient::close_socket() {
    boost::asio::post(m_io_context, [self = shared_from_this()]() {
        if (self->m_socket.is_open()) {
            boost::system::error_code ec;
            self->m_socket.shutdown(tcp::socket::shutdown_both, ec);
            self->m_socket.close(ec);
        }
    });
}

}  // namespace VeritasSync
