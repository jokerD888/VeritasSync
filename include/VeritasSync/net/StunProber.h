#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace VeritasSync {

/**
 * @brief STUN Binding 探测结果
 */
struct StunResult {
    std::string server_host;      // 查询的 STUN 服务器
    std::string reflexive_ip;     // 发现的公网 IP
    uint16_t reflexive_port = 0;  // 发现的公网端口
};

/**
 * @brief 轻量级 STUN Binding Request 客户端
 *
 * 用于多 STUN 并行探测（Multi-STUN Probing），为双 WAN 负载均衡环境
 * 发现更多 reflexive candidate。
 *
 * 每个 STUN 服务器使用独立的 UDP socket（不同源端口 → 路由器可能分配到不同 WAN 线路），
 * 并行发送 RFC 5389 Binding Request，解析 XOR-MAPPED-ADDRESS 响应。
 *
 * 线程安全：所有操作在指定的 io_context 线程中执行。
 */
class StunProber : public std::enable_shared_from_this<StunProber> {
public:
    explicit StunProber(boost::asio::io_context& io);
    ~StunProber();

    // 禁止拷贝
    StunProber(const StunProber&) = delete;
    StunProber& operator=(const StunProber&) = delete;

    using Callback = std::function<void(std::vector<StunResult>)>;

    /**
     * @brief 向多个 STUN 服务器并行发送 Binding Request
     *
     * 为每个服务器创建独立的 UDP socket，发送 Binding Request，
     * 等待响应或超时后回调返回去重后的所有 reflexive 地址。
     *
     * @param servers STUN 服务器列表 (host, port)
     * @param timeout 超时时间
     * @param on_done 完成回调（在 io_context 线程中调用）
     */
    void probe(const std::vector<std::pair<std::string, uint16_t>>& servers,
               std::chrono::milliseconds timeout,
               Callback on_done);

    // --- STUN 协议工具函数（public 供测试使用）---

    /**
     * @brief 构造 STUN Binding Request 报文
     * @param transaction_id 输出：生成的 12 字节 Transaction ID
     * @return 20 字节的 STUN Binding Request
     */
    static std::vector<uint8_t> build_binding_request(std::array<uint8_t, 12>& transaction_id);

    /**
     * @brief 解析 STUN Binding Response，提取 XOR-MAPPED-ADDRESS
     * @param data 响应数据
     * @param size 数据长度
     * @param expected_txn_id 期望的 Transaction ID
     * @param out_ip 输出：reflexive IP 地址字符串
     * @param out_port 输出：reflexive 端口
     * @return true 解析成功
     */
    static bool parse_binding_response(const uint8_t* data, size_t size,
                                       const std::array<uint8_t, 12>& expected_txn_id,
                                       std::string& out_ip, uint16_t& out_port);

private:
    struct ProbeSession;

    boost::asio::io_context& m_io;
};

} // namespace VeritasSync
