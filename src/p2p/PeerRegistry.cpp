#include "VeritasSync/p2p/PeerRegistry.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

void PeerRegistry::add(const std::string& peer_id, std::shared_ptr<PeerController> controller) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_peers[peer_id] = std::move(controller);
}

bool PeerRegistry::try_add(const std::string& peer_id, std::shared_ptr<PeerController> controller) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto [_, inserted] = m_peers.try_emplace(peer_id, std::move(controller));
    return inserted;
}

std::shared_ptr<PeerController> PeerRegistry::remove(const std::string& peer_id) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end()) return nullptr;
    auto controller = std::move(it->second);
    m_peers.erase(it);
    return controller;
}

std::shared_ptr<PeerController> PeerRegistry::find(const std::string& peer_id) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_peers.find(peer_id);
    if (it != m_peers.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<PeerController>> PeerRegistry::collect_connected() const {
    std::vector<std::shared_ptr<PeerController>> result;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto& [peer_id, controller] : m_peers) {
        if (controller->is_connected()) {
            result.push_back(controller);
        }
    }
    return result;
}

std::vector<std::shared_ptr<PeerController>> PeerRegistry::collect_all() const {
    std::vector<std::shared_ptr<PeerController>> result;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    result.reserve(m_peers.size());
    for (auto& [peer_id, controller] : m_peers) {
        result.push_back(controller);
    }
    return result;
}

void PeerRegistry::for_each(ForEachCallback cb) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto& [peer_id, controller] : m_peers) {
        cb(peer_id, controller);
    }
}

bool PeerRegistry::try_reuse_or_evict(const std::string& peer_id, bool force) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_peers.find(peer_id);
    if (it == m_peers.end()) {
        return false;  // 不存在，不跳过
    }

    auto existing_state = it->second->get_state();

    // 非强制模式下，已连接或正在连接的跳过
    if (!force && (existing_state == PeerState::Connected ||
                   existing_state == PeerState::Connecting)) {
        return true;  // 跳过
    }

    // Failed/Disconnected：清理旧的，重新建立
    g_logger->info("[ICE] 对等点 {} 已存在但状态为 {}，重新建立连接",
                   peer_id, static_cast<int>(existing_state));
    it->second->close();
    m_peers.erase(it);

    return false;  // 已清理，不跳过，继续创建
}

void PeerRegistry::close_all() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    for (auto& [peer_id, controller] : m_peers) {
        controller->close();
    }
    m_peers.clear();
}

size_t PeerRegistry::size() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_peers.size();
}

}  // namespace VeritasSync
