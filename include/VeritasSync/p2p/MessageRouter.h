#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace VeritasSync {

class PeerController;

/// 消息类型路由表，替代 dispatch_json_message() 中的 if-else 链
/// 新增消息类型只需一行 register_handler()，无需改 P2PManager 主体
class MessageRouter {
public:
    /// 消息处理器签名
    /// @param payload  JSON payload
    /// @param from_peer  来源 PeerController
    using Handler = std::function<void(nlohmann::json& payload, PeerController* from_peer)>;

    /// 注册消息处理器
    /// @param msg_type  消息类型字符串（如 "share_state"）
    /// @param handler   处理函数
    /// @param receive_only  是否仅在有接收权限时才调用（Destination 或 BiDirectional）
    void register_handler(const std::string& msg_type, Handler handler, bool receive_only = true);

    /// 分发消息
    /// @param msg_type  消息类型
    /// @param payload   JSON payload
    /// @param from_peer 来源 peer
    /// @param can_receive 当前角色是否有接收权限
    /// @return true 表示成功分发给某个 handler，false 表示无匹配
    bool dispatch(const std::string& msg_type, nlohmann::json& payload,
                  PeerController* from_peer, bool can_receive) const;

private:
    struct Route {
        Handler handler;
        bool receive_only;  // true 表示仅 can_receive 时才调用
    };
    std::unordered_map<std::string, Route> m_routes;
};

}  // namespace VeritasSync
