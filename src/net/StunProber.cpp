#include "VeritasSync/net/StunProber.h"
#include "VeritasSync/common/Logger.h"

#include <array>
#include <boost/asio/ip/address_v4.hpp>
#include <cstring>
#include <random>
#include <set>

namespace VeritasSync {

// STUN 协议常量 (RFC 5389)
static constexpr uint16_t STUN_BINDING_REQUEST  = 0x0001;
static constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101;
static constexpr uint32_t STUN_MAGIC_COOKIE     = 0x2112A442;
static constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;
static constexpr uint16_t STUN_ATTR_MAPPED_ADDRESS     = 0x0001;
static constexpr size_t   STUN_HEADER_SIZE      = 20;
static constexpr size_t   STUN_RECV_BUFFER_SIZE = 1024;

// --- 内部：单个 STUN 探测会话 ---
struct StunProber::ProbeSession {
    boost::asio::ip::udp::socket socket;
    boost::asio::ip::udp::endpoint remote_endpoint;
    std::array<uint8_t, 12> transaction_id{};
    std::array<uint8_t, STUN_RECV_BUFFER_SIZE> recv_buffer{};
    boost::asio::ip::udp::endpoint sender_endpoint;
    std::string server_host;

    explicit ProbeSession(boost::asio::io_context& io)
        : socket(io, boost::asio::ip::udp::v4()) {}
};

// --- 构造/析构 ---

StunProber::StunProber(boost::asio::io_context& io)
    : m_io(io) {}

StunProber::~StunProber() = default;

// --- STUN 报文构造 ---

std::vector<uint8_t> StunProber::build_binding_request(std::array<uint8_t, 12>& transaction_id) {
    // 生成随机 Transaction ID
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    for (auto& b : transaction_id) {
        b = static_cast<uint8_t>(dist(rng));
    }

    std::vector<uint8_t> packet(STUN_HEADER_SIZE, 0);

    // Message Type: 0x0001 (Binding Request)
    packet[0] = static_cast<uint8_t>((STUN_BINDING_REQUEST >> 8) & 0xFF);
    packet[1] = static_cast<uint8_t>(STUN_BINDING_REQUEST & 0xFF);

    // Message Length: 0 (no attributes)
    packet[2] = 0;
    packet[3] = 0;

    // Magic Cookie: 0x2112A442
    packet[4] = static_cast<uint8_t>((STUN_MAGIC_COOKIE >> 24) & 0xFF);
    packet[5] = static_cast<uint8_t>((STUN_MAGIC_COOKIE >> 16) & 0xFF);
    packet[6] = static_cast<uint8_t>((STUN_MAGIC_COOKIE >> 8) & 0xFF);
    packet[7] = static_cast<uint8_t>(STUN_MAGIC_COOKIE & 0xFF);

    // Transaction ID (12 bytes)
    std::memcpy(packet.data() + 8, transaction_id.data(), 12);

    return packet;
}

// --- STUN 响应解析 ---

bool StunProber::parse_binding_response(const uint8_t* data, size_t size,
                                        const std::array<uint8_t, 12>& expected_txn_id,
                                        std::string& out_ip, uint16_t& out_port) {
    if (size < STUN_HEADER_SIZE) return false;

    // 验证 Message Type: Binding Response (0x0101)
    uint16_t msg_type = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (msg_type != STUN_BINDING_RESPONSE) return false;

    // 验证 Magic Cookie
    uint32_t cookie = (static_cast<uint32_t>(data[4]) << 24) |
                      (static_cast<uint32_t>(data[5]) << 16) |
                      (static_cast<uint32_t>(data[6]) << 8) |
                      static_cast<uint32_t>(data[7]);
    if (cookie != STUN_MAGIC_COOKIE) return false;

    // 验证 Transaction ID
    if (std::memcmp(data + 8, expected_txn_id.data(), 12) != 0) return false;

    // 获取 Message Length
    uint16_t msg_len = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    if (size < STUN_HEADER_SIZE + msg_len) return false;

    // 遍历属性，查找 XOR-MAPPED-ADDRESS 或 MAPPED-ADDRESS
    size_t offset = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + msg_len;

    while (offset + 4 <= end) {
        uint16_t attr_type = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        uint16_t attr_len  = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4;

        if (offset + attr_len > end) break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            // XOR-MAPPED-ADDRESS 解析
            uint8_t family = data[offset + 1];
            if (family != 0x01) {  // 仅支持 IPv4
                offset += (attr_len + 3) & ~3u;  // 4 字节对齐
                continue;
            }

            // X-Port: port XOR (magic_cookie >> 16)
            uint16_t xport = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
            out_port = xport ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);

            // X-Address: address XOR magic_cookie (IPv4)
            uint32_t xaddr = (static_cast<uint32_t>(data[offset + 4]) << 24) |
                             (static_cast<uint32_t>(data[offset + 5]) << 16) |
                             (static_cast<uint32_t>(data[offset + 6]) << 8) |
                             static_cast<uint32_t>(data[offset + 7]);
            uint32_t addr = xaddr ^ STUN_MAGIC_COOKIE;

            out_ip = boost::asio::ip::address_v4(addr).to_string();

            return true;
        }

        if (attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8) {
            // MAPPED-ADDRESS 回退解析（无 XOR）
            uint8_t family = data[offset + 1];
            if (family != 0x01) {
                offset += (attr_len + 3) & ~3u;
                continue;
            }

            out_port = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];

            uint32_t addr = (static_cast<uint32_t>(data[offset + 4]) << 24) |
                            (static_cast<uint32_t>(data[offset + 5]) << 16) |
                            (static_cast<uint32_t>(data[offset + 6]) << 8) |
                            static_cast<uint32_t>(data[offset + 7]);

            out_ip = boost::asio::ip::address_v4(addr).to_string();

            return true;
        }

        // 跳到下一个属性（4 字节对齐）
        offset += (attr_len + 3) & ~3u;
    }

    return false;  // 未找到地址属性
}

// --- 并行探测 ---

void StunProber::probe(const std::vector<std::pair<std::string, uint16_t>>& servers,
                       std::chrono::milliseconds timeout,
                       Callback on_done) {
    if (servers.empty()) {
        if (on_done) on_done({});
        return;
    }

    // 共享状态：收集所有探测结果
    struct SharedState {
        std::vector<StunResult> results;
        size_t pending_count = 0;
        bool timer_fired = false;
        Callback callback;
        std::shared_ptr<boost::asio::steady_timer> timer;
        std::vector<std::shared_ptr<ProbeSession>> sessions;
        std::mutex mutex;

        void try_complete() {
            // 幂等保护：callback 被 move 后为空，防止二次调用
            if (!callback) return;
            // 去重：相同 IP:port 只保留一个
            std::set<std::string> seen;
            std::vector<StunResult> deduped;
            for (auto& r : results) {
                std::string key = r.reflexive_ip + ":" + std::to_string(r.reflexive_port);
                if (seen.insert(key).second) {
                    deduped.push_back(std::move(r));
                }
            }
            auto cb = std::move(callback);
            cb(std::move(deduped));
        }

        // 一次探测完成（成功或失败），减少 pending 计数
        // 必须在持有 mutex 的情况下调用
        void mark_one_done() {
            if (--pending_count == 0 && !timer_fired) {
                timer_fired = true;
                timer->cancel();
                try_complete();
            }
        }
    };

    auto state = std::make_shared<SharedState>();
    state->pending_count = servers.size();
    state->callback = std::move(on_done);
    state->timer = std::make_shared<boost::asio::steady_timer>(m_io);

    // 设置超时定时器
    state->timer->expires_after(timeout);
    state->timer->async_wait([state](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) return;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->timer_fired) return;
        state->timer_fired = true;

        if (g_logger) {
            g_logger->debug("[StunProber] 超时，已收集 {} 个结果", state->results.size());
        }

        // 关闭所有还在等待的 socket
        for (auto& session : state->sessions) {
            boost::system::error_code close_ec;
            session->socket.close(close_ec);
        }

        state->try_complete();
    });

    // 共享 DNS 解析器（stateless，可安全地被多个 async_resolve 共用）
    auto resolver = std::make_shared<boost::asio::ip::udp::resolver>(m_io);

    // 为每个 STUN 服务器启动独立探测
    for (const auto& [host, port] : servers) {
        auto session = std::make_shared<ProbeSession>(m_io);
        session->server_host = host;
        state->sessions.push_back(session);

        // DNS 解析
        resolver->async_resolve(
            host, std::to_string(port),
            [state, session, resolver, self = shared_from_this()](
                const boost::system::error_code& ec,
                boost::asio::ip::udp::resolver::results_type results) {
                if (ec) {
                    if (g_logger) {
                        g_logger->debug("[StunProber] DNS 解析失败 {}: {}", session->server_host, ec.message());
                    }
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->mark_one_done();
                    return;
                }

                session->remote_endpoint = *results.begin();

                // 构造 Binding Request
                auto packet = build_binding_request(session->transaction_id);

                // 发送
                session->socket.async_send_to(
                    boost::asio::buffer(packet), session->remote_endpoint,
                    [state, session, self](const boost::system::error_code& send_ec, size_t /*bytes*/) {
                        if (send_ec) {
                            if (g_logger) {
                                g_logger->debug("[StunProber] 发送失败 {}: {}",
                                               session->server_host, send_ec.message());
                            }
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->mark_one_done();
                            return;
                        }

                        // 接收响应
                        session->socket.async_receive_from(
                            boost::asio::buffer(session->recv_buffer),
                            session->sender_endpoint,
                            [state, session, self](const boost::system::error_code& recv_ec, size_t bytes) {
                                if (!recv_ec && bytes > 0) {
                                    std::string ip;
                                    uint16_t port = 0;
                                    if (parse_binding_response(session->recv_buffer.data(), bytes,
                                                               session->transaction_id, ip, port)) {
                                        if (g_logger) {
                                            g_logger->info("[StunProber] {} -> reflexive {}:{}",
                                                          session->server_host, ip, port);
                                        }
                                        std::lock_guard<std::mutex> lock(state->mutex);
                                        state->results.push_back({session->server_host, ip, port});
                                    }
                                }

                                std::lock_guard<std::mutex> lock(state->mutex);
                                state->mark_one_done();
                            });
                    });
            });
    }
}

} // namespace VeritasSync
