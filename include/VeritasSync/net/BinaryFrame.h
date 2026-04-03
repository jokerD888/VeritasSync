#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

/**
 * @brief 统一二进制 Frame 协议
 * 
 * 帧结构:
 * +--------+----------+-------------+-----------+
 * | Magic  | MsgType  | PayloadLen  |  Payload  |
 * | 2 bytes| 1 byte   | 4 bytes     |  N bytes  |
 * +--------+----------+-------------+-----------+
 * 
 * - Magic: 0x56 0x53 ("VS" for VeritasSync)
 * - MsgType: 消息类型
 * - PayloadLen: 载荷长度 (网络字节序)
 * - Payload: JSON 或 二进制数据
 */
class BinaryFrame {
public:
    // 魔数
    static constexpr uint8_t MAGIC_BYTE_1 = 0x56; // 'V'
    static constexpr uint8_t MAGIC_BYTE_2 = 0x53; // 'S'
    
    // 消息类型
    enum class MessageType : uint8_t {
        // JSON 消息 (0x01 - 0x7F)
        JSON = 0x01,
        
        // 二进制消息 (0x80 - 0xFF)
        BINARY_CHUNK = 0x80,
        BINARY_ACK = 0x81,
    };
    
    // 帧头大小
    static constexpr size_t HEADER_SIZE = 7; // 2 + 1 + 4
    
    // S-5 安全修复: 单帧最大载荷大小 64 MB
    // 防止恶意节点发送超大 payload_len 导致 OOM
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;
    
    /**
     * @brief 编码 Frame
     * @param type 消息类型
     * @param payload 载荷数据
     * @return 编码后的完整帧，payload 超限时返回空串
     */
    static std::string encode(MessageType type, const std::string& payload) {
        // S-5: 发送端也做校验，防止编程错误导致发出超大帧
        if (payload.size() > MAX_PAYLOAD_SIZE) {
            return "";
        }
        
        std::string frame;
        frame.reserve(HEADER_SIZE + payload.size());
        
        // Magic
        frame.push_back(static_cast<char>(MAGIC_BYTE_1));
        frame.push_back(static_cast<char>(MAGIC_BYTE_2));
        
        // MsgType
        frame.push_back(static_cast<char>(type));
        
        // PayloadLen (网络字节序 - 大端)
        uint32_t len = static_cast<uint32_t>(payload.size());
        frame.push_back(static_cast<char>((len >> 24) & 0xFF));
        frame.push_back(static_cast<char>((len >> 16) & 0xFF));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
        
        // Payload
        frame.append(payload);
        
        return frame;
    }
    
    /**
     * @brief 编码 JSON 消息
     */
    static std::string encode_json(const std::string& json_payload) {
        return encode(MessageType::JSON, json_payload);
    }
    
    /**
     * @brief 编码二进制块消息
     */
    static std::string encode_binary_chunk(const std::string& binary_payload) {
        return encode(MessageType::BINARY_CHUNK, binary_payload);
    }
    
    /**
     * @brief 解析结果
     */
    struct DecodeResult {
        MessageType type;
        std::string payload;
    };
    
    /**
     * @brief 解码 Frame
     * @param data 数据指针
     * @param size 数据大小
     * @return 解析结果，失败返回 nullopt
     */
    static std::optional<DecodeResult> decode(const char* data, size_t size) {
        // 检查最小长度
        if (size < HEADER_SIZE) {
            return std::nullopt;
        }
        
        // 检查魔数
        if (static_cast<uint8_t>(data[0]) != MAGIC_BYTE_1 ||
            static_cast<uint8_t>(data[1]) != MAGIC_BYTE_2) {
            return std::nullopt;
        }
        
        // 读取消息类型
        MessageType type = static_cast<MessageType>(static_cast<uint8_t>(data[2]));
        
        // 读取载荷长度 (大端)
        uint32_t payload_len = 
            (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24) |
            (static_cast<uint32_t>(static_cast<uint8_t>(data[4])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(data[5])) << 8) |
            static_cast<uint32_t>(static_cast<uint8_t>(data[6]));
        
        // S-5 安全修复: 载荷长度上限校验，防止恶意超大帧导致 OOM
        if (payload_len > MAX_PAYLOAD_SIZE) {
            return std::nullopt;
        }
        
        // 检查完整性
        if (size < HEADER_SIZE + payload_len) {
            return std::nullopt;
        }
        
        // 提取载荷
        DecodeResult result;
        result.type = type;
        result.payload = std::string(data + HEADER_SIZE, payload_len);
        
        return result;
    }
    
    /**
     * @brief 解码 Frame (string 版本)
     */
    static std::optional<DecodeResult> decode(const std::string& data) {
        return decode(data.data(), data.size());
    }
    
    /**
     * @brief 检查是否为 JSON 消息
     */
    static bool is_json_message(MessageType type) {
        return static_cast<uint8_t>(type) < 0x80;
    }
    
    /**
     * @brief 检查是否为二进制消息
     */
    static bool is_binary_message(MessageType type) {
        return static_cast<uint8_t>(type) >= 0x80;
    }
};

// ═══════════════════════════════════════════════════════════════
// 兼容旧代码的常量（后续全面迁移到 BinaryFrame::MessageType 后可移除）
// ═══════════════════════════════════════════════════════════════
static const uint8_t MSG_TYPE_JSON = 0x01;
static const uint8_t MSG_TYPE_BINARY_CHUNK = 0x02;
static const uint8_t MSG_TYPE_PING = 0x03;  // 心跳包：保持 NAT 映射存活

// ═══════════════════════════════════════════════════════════════
// 统一的二进制序列化/反序列化工具函数
// 用于协议封包/解包中的网络字节序读写
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
