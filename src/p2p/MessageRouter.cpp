#include "VeritasSync/p2p/MessageRouter.h"
#include "VeritasSync/common/Logger.h"

#include <nlohmann/json.hpp>

namespace VeritasSync {

void MessageRouter::register_handler(const std::string& msg_type, Handler handler, bool receive_only) {
    m_routes[msg_type] = {std::move(handler), receive_only};
}

bool MessageRouter::dispatch(const std::string& msg_type, nlohmann::json& payload,
                              PeerController* from_peer, bool can_receive) const {
    auto it = m_routes.find(msg_type);
    if (it == m_routes.end()) {
        return false;  // 无匹配
    }

    const auto& route = it->second;

    // receive_only 的消息需要 can_receive 权限
    if (route.receive_only && !can_receive) {
        return false;
    }

    route.handler(payload, from_peer);
    return true;
}

}  // namespace VeritasSync
