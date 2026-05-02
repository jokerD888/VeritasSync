#pragma once

/**
 * @brief Tracker 信令协议常量
 * 
 * 共享定义，由 TrackerClient 和 TrackerServer 共同使用。
 * 消除以前在两处独立维护导致的重复和不一致风险。
 */
namespace SignalProto {
constexpr const char* MSG_TYPE = "type";
constexpr const char* MSG_PAYLOAD = "payload";
constexpr const char* TYPE_REGISTER = "REGISTER";
constexpr const char* TYPE_REG_ACK = "REG_ACK";
constexpr const char* TYPE_PEER_JOIN = "PEER_JOIN";
constexpr const char* TYPE_PEER_LEAVE = "PEER_LEAVE";
constexpr const char* TYPE_SIGNAL = "SIGNAL";
}  // namespace SignalProto
