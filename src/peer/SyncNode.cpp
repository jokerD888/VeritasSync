#include "VeritasSync/SyncNode.h"

#include <filesystem>
#include <iostream>

#include "VeritasSync/EncodingUtils.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

namespace VeritasSync {

SyncNode::SyncNode(SyncTask task, const Config& global_config)
    : m_task(std::move(task)), m_global_config(global_config) {}

SyncNode::~SyncNode() = default;

std::shared_ptr<P2PManager> SyncNode::get_p2p() { return m_p2p_manager; }
std::string SyncNode::get_key() const { return m_task.sync_key; }
std::string SyncNode::get_root_path() const { return m_task.sync_folder; }

void SyncNode::start() {
    g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
    g_logger->info("[Config] Role: {}", m_task.role);
    g_logger->info("[Config] Sync Folder: {}", m_task.sync_folder);

    // 1. 使用 EncodingUtils 转换路径
    std::filesystem::path sync_path = Utf8ToPath(m_task.sync_folder);

    SyncRole role;
    if (m_task.role == "source") {
        role = SyncRole::Source;
    } else if (m_task.role == "destination") {
        role = SyncRole::Destination;
    } else {
        g_logger->error("Invalid role: '{}' for task '{}'.", m_task.role, m_task.sync_key);
        return;
    }

    // 开启文件监控的条件
    bool enable_watcher = (role == SyncRole::Source) || (m_task.mode == SyncMode::BiDirectional);

    // 2. 确保目录存在
    std::error_code ec;
    if (!std::filesystem::exists(sync_path, ec)) {
        std::filesystem::create_directories(sync_path, ec);
        if (ec) {
            g_logger->error("[SyncNode] 创建同步目录失败: {}", ec.message());
            return;
        }
        g_logger->info("[SyncNode] 创建同步目录成功");
    } else {
        g_logger->info("[SyncNode] 使用现有同步目录");
    }

    // 3. 创建 P2PManager
    m_p2p_manager = P2PManager::create();

    // 4. 创建 TrackerClient
    m_tracker_client = std::make_shared<TrackerClient>(m_global_config.tracker_host, m_global_config.tracker_port);

    // 5. 互相注入依赖
    m_tracker_client->set_p2p_manager(m_p2p_manager.get());
    m_p2p_manager->set_tracker_client(m_tracker_client.get());

    // 6. 配置 P2PManager
    m_p2p_manager->set_role(role);
    m_p2p_manager->set_encryption_key(m_task.sync_key);
    m_p2p_manager->set_mode(m_task.mode);

    // 配置 STUN
    if (!m_global_config.stun_host.empty()) {
        g_logger->info("[Config] Using STUN server at {}:{}", m_global_config.stun_host, m_global_config.stun_port);
        m_p2p_manager->set_stun_config(m_global_config.stun_host, m_global_config.stun_port);
    }

    // 配置 TURN
    if (!m_global_config.turn_host.empty()) {
        g_logger->info("[Config] Using TURN server at {}:{}", m_global_config.turn_host, m_global_config.turn_port);
        m_p2p_manager->set_turn_config(m_global_config.turn_host, m_global_config.turn_port,
                                       m_global_config.turn_username, m_global_config.turn_password);
    }

    // 7. 创建 StateManager
    m_state_manager =
        std::make_unique<StateManager>(m_task.sync_folder, *m_p2p_manager, enable_watcher, m_task.sync_key);

    // 8. 注入 StateManager
    m_p2p_manager->set_state_manager(m_state_manager.get());

    // 9. 初始扫描
    m_state_manager->scan_directory();

    // 10. 启动信令连接
    g_logger->info("[{}] --- Phase 1: Contacting Tracker ---", m_task.sync_key);

    m_tracker_client->connect(m_task.sync_key, [this](std::vector<std::string> peer_list) {
        g_logger->info("[{}] --- Phase 2: P2P (ICE) connection ---", m_task.sync_key);
        m_p2p_manager->connect_to_peers(peer_list);
    });
}

}  // namespace VeritasSync