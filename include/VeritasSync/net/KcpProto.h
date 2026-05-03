#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// KCP 消息类型常量
// KCP 层消息格式: [1字节类型] + [payload]
// ═══════════════════════════════════════════════════════════════
inline constexpr uint8_t MSG_TYPE_JSON         = 0x01;
inline constexpr uint8_t MSG_TYPE_BINARY_CHUNK = 0x80;
inline constexpr uint8_t MSG_TYPE_PING         = 0x03;  // 心跳包：保持 NAT 映射存活

// ═══════════════════════════════════════════════════════════════
// 二进制序列化/反序列化工具函数
// 网络字节序（大端）读写
// ═══════════════════════════════════════════════════════════════
inline void append_uint16(std::string& s, uint16_t val) {
    uint16_t net_val = boost::asio::detail::socket_ops::host_to_network_short(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}

inline void append_uint32(std::string& s, uint32_t val) {
    uint32_t net_val = boost::asio::detail::socket_ops::host_to_network_long(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}

inline uint16_t read_uint16(const char*& data, size_t& len) {
    if (len < sizeof(uint16_t)) return 0;
    uint16_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_short(net_val);
}

inline uint32_t read_uint32(const char*& data, size_t& len) {
    if (len < sizeof(uint32_t)) return 0;
    uint32_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_long(net_val);
}

} // namespace VeritasSync
