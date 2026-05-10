#include "VeritasSync/net/LanDiscovery.h"
#include <nlohmann/json.hpp>
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

using boost::asio::ip::udp;
using boost::asio::ip::tcp;

// ═══════════════════════════════════════════════════════════════
// 工具：心跳包序列化/反序列化
// ═══════════════════════════════════════════════════════════════

std::string LanDiscovery::make_heartbeat_payload(const std::string& device_id,
                                                 const std::string& sync_key,
                                                 uint16_t signaling_port,
                                                 uint16_t kcp_port) {
    nlohmann::json j;
    j["device_id"] = device_id;
    j["sync_key"] = sync_key;
    j["signaling_port"] = signaling_port;
    j["kcp_port"] = kcp_port;
    return j.dump();
}

bool LanDiscovery::parse_heartbeat(const std::string& payload,
                                   std::string& out_device_id,
                                   std::string& out_sync_key,
                                   uint16_t& out_signaling_port,
                                   uint16_t& out_kcp_port) {
    try {
        auto j = nlohmann::json::parse(payload);
        if (!j.contains("device_id") || !j.contains("sync_key") || !j.contains("signaling_port"))
            return false;
        j.at("device_id").get_to(out_device_id);
        j.at("sync_key").get_to(out_sync_key);
        out_signaling_port = static_cast<uint16_t>(j.at("signaling_port").get<int>());
        out_kcp_port = j.contains("kcp_port") ? static_cast<uint16_t>(j.at("kcp_port").get<int>()) : 0;
        return true;
    } catch (...) {
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════
// 工厂方法
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<LanDiscovery> LanDiscovery::create(
    boost::asio::io_context& io_context,
    const std::string& device_id,
    const std::string& sync_key,
    LanDiscoveryCallbacks callbacks,
    uint16_t multicast_port,
    const std::string& multicast_group,
    uint16_t kcp_udp_port)
{
    return std::shared_ptr<LanDiscovery>(new LanDiscovery(
        io_context, device_id, sync_key, std::move(callbacks),
        multicast_port, multicast_group, kcp_udp_port));
}

// ═══════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════

LanDiscovery::LanDiscovery(boost::asio::io_context& io_context,
                           const std::string& device_id,
                           const std::string& sync_key,
                           LanDiscoveryCallbacks callbacks,
                           uint16_t multicast_port,
                           const std::string& multicast_group,
                           uint16_t kcp_udp_port)
    : m_io_context(io_context)
    , m_device_id(device_id)
    , m_sync_key(sync_key)
    , m_callbacks(std::move(callbacks))
    , m_multicast_port(multicast_port)
    , m_multicast_group(multicast_group)
    , m_kcp_udp_port(kcp_udp_port)
    , m_udp_socket(io_context)
    , m_tcp_acceptor(io_context)
    , m_heartbeat_timer(io_context)
    , m_timeout_timer(io_context)
{
}

LanDiscovery::~LanDiscovery() {
    stop();
}

// ═══════════════════════════════════════════════════════════════
// start / stop
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::start() {
    if (m_running.exchange(true)) return;

    if (g_logger) g_logger->info("[LAN] 启动局域网发现 (组播 {}:{})", m_multicast_group, m_multicast_port);

    // 1. 创建 UDP socket 并加入组播组
    boost::system::error_code ec;
    m_udp_socket.open(udp::v4(), ec);
    if (ec) {
        if (g_logger) g_logger->error("[LAN] UDP socket 打开失败: {}", ec.message());
        m_running = false;
        return;
    }

    m_udp_socket.set_option(udp::socket::reuse_address(true), ec);
    m_udp_socket.bind(udp::endpoint(udp::v4(), m_multicast_port), ec);
    if (ec) {
        if (g_logger) g_logger->error("[LAN] UDP 绑定 {} 失败: {}", m_multicast_port, ec.message());
        m_running = false;
        return;
    }

    // 启用多播回环，允许同一台机器上的不同进程互相收发多播
    m_udp_socket.set_option(boost::asio::ip::multicast::enable_loopback(true), ec);

    m_udp_socket.set_option(boost::asio::ip::multicast::join_group(
        boost::asio::ip::make_address(m_multicast_group)), ec);
    if (ec) {
        if (g_logger) g_logger->warn("[LAN] 加入组播组失败（可能被防火墙拦截）: {}", ec.message());
        // 继续运行，至少可以发心跳
    }

    // 2. 启动 TCP 信令服务（自动分配端口）
    start_tcp_signaling_server();

    // 3. 启动 UDP 接收
    start_multicast_receive();

    // 4. 启动心跳发送
    start_heartbeat();

    // 5. 启动超时检测
    start_timeout_check();
}

void LanDiscovery::stop() {
    if (!m_running.exchange(false)) return;

    if (g_logger) g_logger->info("[LAN] 停止局域网发现");

    boost::system::error_code ec;
    m_heartbeat_timer.cancel();
    m_timeout_timer.cancel();
    m_udp_socket.close(ec);
    m_tcp_acceptor.close(ec);

    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        for (auto& [id, tp] : m_peers) {
            if (tp.signaling_ch && tp.signaling_ch->socket) {
                tp.signaling_ch->socket->close(ec);
            }
        }
        m_peers.clear();
    }
}

// ═══════════════════════════════════════════════════════════════
// TCP 信令服务
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::start_tcp_signaling_server() {
    boost::system::error_code ec;
    // 绑定到 0 端口自动分配，再查具体端口
    m_tcp_acceptor.open(tcp::v4(), ec);
    if (ec) {
        if (g_logger) g_logger->error("[LAN] TCP acceptor 打开失败: {}", ec.message());
        return;
    }
    m_tcp_acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    m_tcp_acceptor.bind(tcp::endpoint(tcp::v4(), 0), ec);
    if (ec) {
        if (g_logger) g_logger->error("[LAN] TCP 信令端口绑定失败: {}", ec.message());
        return;
    }
    m_signaling_port = m_tcp_acceptor.local_endpoint(ec).port();
    if (ec) {
        if (g_logger) g_logger->error("[LAN] 获取信令端口失败: {}", ec.message());
        return;
    }
    m_tcp_acceptor.listen(8, ec);
    if (!ec) {
        if (g_logger) g_logger->info("[LAN] TCP 信令服务监听端口: {}", m_signaling_port);
        do_tcp_accept();
    }
}

void LanDiscovery::do_tcp_accept() {
    if (!m_running) return;
    auto sock = std::make_shared<tcp::socket>(m_io_context);
    m_tcp_acceptor.async_accept(*sock, [self = shared_from_this(), sock](boost::system::error_code ec) {
        if (!ec && self->m_running) {
            self->handle_tcp_session(sock);
        }
        self->do_tcp_accept();
    });
}

void LanDiscovery::handle_tcp_session(std::shared_ptr<tcp::socket> sock) {
    // TCP 信令消息格式：一行 signal_type|payload_json
    auto ch = std::make_shared<SignalingChannel>();
    ch->socket = sock;
    ch->connected_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    do_signaling_read(ch);
}

void LanDiscovery::do_signaling_read(std::shared_ptr<SignalingChannel> ch) {
    if (!m_running) return;
    auto self = shared_from_this();
    boost::asio::async_read_until(*ch->socket, ch->read_buf, '\n',
        [self, ch](boost::system::error_code ec, size_t /*length*/) {
            if (ec || !self->m_running) {
                if (ec != boost::asio::error::operation_aborted && ec != boost::asio::error::eof) {
                    if (g_logger) g_logger->debug("[LAN] 信令读取结束: {}", ec.message());
                }
                return;
            }
            // 解析消息
            std::istream is(&ch->read_buf);
            std::string line;
            std::getline(is, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // 格式: signal_type|payload_json
            auto sep = line.find('|');
            if (sep != std::string::npos) {
                std::string signal_type = line.substr(0, sep);
                std::string payload = line.substr(sep + 1);
                if (signal_type == "hello") {
                    // 第一行是 peer_id 注册
                    ch->peer_id = payload;
                    if (g_logger) g_logger->debug("[LAN] 收到信令连接: {}", payload);
                } else if (!ch->peer_id.empty()) {
                    // 转发给回调
                    if (self->m_callbacks.on_signaling_received)
                        self->m_callbacks.on_signaling_received(ch->peer_id, signal_type, payload);
                }
            }
            self->do_signaling_read(ch);
        });
}

// ═══════════════════════════════════════════════════════════════
// 信令发送（由 P2PManager 调用）
// ═══════════════════════════════════════════════════════════════

bool LanDiscovery::send_signaling(const std::string& peer_device_id,
                                  const std::string& signal_type,
                                  const std::string& payload)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    auto it = m_peers.find(peer_device_id);
    if (it == m_peers.end() || !it->second.signaling_ch || !it->second.signaling_ch->socket) {
        return false;
    }
    std::string msg = signal_type + "|" + payload + "\n";
    do_signaling_write(it->second.signaling_ch, msg);
    return true;
}

void LanDiscovery::do_signaling_write(std::shared_ptr<SignalingChannel> ch, std::string msg) {
    if (!ch->socket || !ch->socket->is_open()) return;
    auto self = shared_from_this();
    // 重要：async_write 的 buffer 数据必须存活到 handler 被调用。
    // 通过 shared_ptr 持有 string，回调释放时自动清理。
    auto buf = std::make_shared<std::string>(std::move(msg));
    boost::asio::async_write(*ch->socket, boost::asio::buffer(*buf),
        [self, ch, buf](boost::system::error_code ec, size_t) {
            if (ec && ec != boost::asio::error::operation_aborted) {
                if (g_logger) g_logger->debug("[LAN] 信令写入失败: {}", ec.message());
            }
            // buf 在 lambda 释放时自动析构
        });
}

// ═══════════════════════════════════════════════════════════════
// UDP 多播接收
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::start_multicast_receive() {
    do_multicast_receive();
}

void LanDiscovery::do_multicast_receive() {
    if (!m_running) return;
    auto self = shared_from_this();
    m_udp_socket.async_receive_from(
        boost::asio::buffer(m_recv_buffer), m_remote_endpoint,
        [self](boost::system::error_code ec, size_t length) {
            if (!ec && self->m_running) {
                std::string data(self->m_recv_buffer.data(), length);
                self->handle_heartbeat(data, self->m_remote_endpoint);
            }
            self->do_multicast_receive();
        });
}

void LanDiscovery::handle_heartbeat(const std::string& data,
                                    const udp::endpoint& sender)
{
    std::string dev_id, sync_key;
    uint16_t sig_port = 0, kcp_port = 0;
    if (!parse_heartbeat(data, dev_id, sync_key, sig_port, kcp_port))
        return;

    // 跳过自己的心跳
    if (dev_id == m_device_id)
        return;

    // 只处理匹配 sync_key 的心跳
    if (sync_key != m_sync_key)
        return;

    std::string peer_ip = sender.address().to_string();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    bool is_new = false;
    bool ip_changed = false;
    LanPeerInfo info;

    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers.find(dev_id);
        if (it == m_peers.end()) {
            is_new = true;
        } else {
            ip_changed = (it->second.info.lan_ip != peer_ip ||
                          it->second.info.signaling_port != sig_port);
        }

        // 更新或插入
        info.device_id = dev_id;
        info.lan_ip = peer_ip;
        info.signaling_port = sig_port;
        info.kcp_port = kcp_port;
        info.last_seen_ms = now_ms;
        m_last_activity_time.store(now_ms, std::memory_order_release);

        TrackedPeer& tp = m_peers[dev_id];
        bool need_tcp_connect = is_new && tp.signaling_ch == nullptr;
        tp.info = info;

        // 如果是新发现的 peer，启动 TCP 信令连接
        if (need_tcp_connect) {
            connect_signaling(dev_id, peer_ip, sig_port);
        } else if (ip_changed) {
            // IP 变了，重建 TCP 连接
            if (tp.signaling_ch && tp.signaling_ch->socket) {
                tp.signaling_ch->socket->close();
            }
            tp.signaling_ch.reset();
            connect_signaling(dev_id, peer_ip, sig_port);
        }
    }

    // 触发回调（在锁外，避免死锁）
    if (is_new || ip_changed) {
        if (m_callbacks.on_peer_discovered)
            m_callbacks.on_peer_discovered(info);
    }
}

// ═══════════════════════════════════════════════════════════════
// TCP 信令客户端（主动连接对端）
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::connect_signaling(const std::string& peer_id, const std::string& ip, uint16_t port) {
    auto ch = std::make_shared<SignalingChannel>();
    ch->peer_id = peer_id;
    ch->socket = std::make_shared<tcp::socket>(m_io_context);
    ch->connected_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    auto self = shared_from_this();
    auto ep = tcp::endpoint(boost::asio::ip::make_address(ip), port);
    ch->socket->async_connect(ep,
        [self, ch, peer_id](boost::system::error_code ec) {
            if (ec || !self->m_running) {
                if (g_logger) g_logger->debug("[LAN] 信令连接 {}:{} 失败: {}", 
                    ch->socket ? ch->socket->remote_endpoint().address().to_string() : "?", 
                    ch->socket ? ch->socket->remote_endpoint().port() : 0,
                    ec.message());
                return;
            }
            // 发送 hello 消息注册本端身份
            std::string hello = "hello|" + self->m_device_id + "\n";
            self->do_signaling_write(ch, hello);
            // 保存 channel
            {
                std::lock_guard<std::mutex> lock(self->m_peers_mutex);
                auto it = self->m_peers.find(peer_id);
                if (it != self->m_peers.end()) {
                    it->second.signaling_ch = ch;
                }
            }
            // 开始读取
            self->do_signaling_read(ch);
        });
}

// ═══════════════════════════════════════════════════════════════
// 心跳发送
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::start_heartbeat() {
    do_send_heartbeat();
}

void LanDiscovery::do_send_heartbeat() {
    if (!m_running) return;

    // 自适应心跳：如果最近有 peer 的数据收发活动（5 秒内），跳过本次心跳发送
    // 因为数据传输本身就是"在线"的证明，无需用心跳验证
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool has_recent_activity = (now_ms - m_last_activity_time.load(std::memory_order_acquire))
                               < HEARTBEAT_IDLE_THRESHOLD_MS;

    if (!has_recent_activity) {
        auto payload = std::make_shared<std::string>(
            make_heartbeat_payload(m_device_id, m_sync_key, m_signaling_port, m_kcp_udp_port));
        m_multicast_endpoint = udp::endpoint(
            boost::asio::ip::make_address(m_multicast_group), m_multicast_port);

        auto self = shared_from_this();
        m_udp_socket.async_send_to(boost::asio::buffer(*payload), m_multicast_endpoint,
            [self, payload](boost::system::error_code ec, size_t) {
                if (ec && ec != boost::asio::error::operation_aborted) {
                    if (g_logger) g_logger->debug("[LAN] 心跳发送失败: {}", ec.message());
                }
            });
    }

    // 不管是否跳过，都要调度下一次检查
    m_heartbeat_timer.expires_after(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    m_heartbeat_timer.async_wait([self = shared_from_this()](boost::system::error_code ec) {
        if (!ec && self->m_running) self->do_send_heartbeat();
    });
}

// ═══════════════════════════════════════════════════════════════
// 超时检测
// ═══════════════════════════════════════════════════════════════

void LanDiscovery::start_timeout_check() {
    check_timeouts();
}

void LanDiscovery::check_timeouts() {
    if (!m_running) return;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<std::string> stale_peers;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        for (auto it = m_peers.begin(); it != m_peers.end(); ) {
            if (now_ms - it->second.info.last_seen_ms > PEER_TIMEOUT_MS) {
                stale_peers.push_back(it->first);
                if (it->second.signaling_ch && it->second.signaling_ch->socket)
                    it->second.signaling_ch->socket->close();
                it = m_peers.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& pid : stale_peers) {
        if (g_logger) g_logger->info("[LAN] 对等点离线: {}", pid);
        if (m_callbacks.on_peer_lost)
            m_callbacks.on_peer_lost(pid);
    }

    auto self = shared_from_this();
    m_timeout_timer.expires_after(std::chrono::milliseconds(TIMEOUT_CHECK_INTERVAL_MS));
    m_timeout_timer.async_wait([self](boost::system::error_code ec) {
        if (!ec && self->m_running) self->check_timeouts();
    });
}

// ═══════════════════════════════════════════════════════════════
// 获取当前已知对等点
// ═══════════════════════════════════════════════════════════════

std::vector<LanPeerInfo> LanDiscovery::get_peers() const {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    std::vector<LanPeerInfo> result;
    result.reserve(m_peers.size());
    for (const auto& [id, tp] : m_peers) {
        result.push_back(tp.info);
    }
    return result;
}

void LanDiscovery::refresh_peer(const std::string& peer_device_id) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    auto it = m_peers.find(peer_device_id);
    if (it != m_peers.end()) {
        it->second.info.last_seen_ms = now_ms;
    }
    m_last_activity_time.store(now_ms, std::memory_order_release);
}

} // namespace VeritasSync
