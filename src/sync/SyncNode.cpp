#include "VeritasSync/sync/SyncNode.h"

#include <filesystem>
#include <iostream>
// <memory> 已在头文件中包含，这里不需要重复

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/P2PManager.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/p2p/TrackerClient.h"

namespace VeritasSync {

SyncNode::SyncNode(SyncTask task, const Config& global_config)
    : m_task(std::move(task)), m_global_config(global_config) {}

SyncNode::~SyncNode() {
    // 直接调用 stop() 复用清理逻辑（DRY 原则）
    // stop() 内部有 m_started 检查，防止重复清理
    stop();
}

std::shared_ptr<P2PManager> SyncNode::get_p2p() { 
    // C++20 原子智能指针：直接 load
    return m_p2p_manager.load();
}
std::string SyncNode::get_key() const { return m_task.sync_key; }
std::string SyncNode::get_root_path() const { return m_task.sync_folder; }

bool SyncNode::is_tracker_online() const {
    // C++20 原子智能指针：直接 load
    auto p2p = m_p2p_manager.load();
    auto tracker = m_tracker_client.load();
    return p2p && tracker && tracker->is_connected();
}

bool SyncNode::is_started() const {
    return m_started.load();
}

void SyncNode::stop() {
    if (!m_started.exchange(false)) {
        return;  // 已经停止或从未启动
    }
    
    // 立即标记停止状态（防止僵尸回调）
    m_is_stopping.store(true, std::memory_order_release);
    
    g_logger->info("[{}] Stopping Sync Task...", m_task.sync_key);
    
    // 按照析构函数的相同顺序停止
    auto tracker = m_tracker_client.load();
    if (tracker) {
        tracker->stop();
    }
    
    m_state_manager.reset();
    m_p2p_manager.store(nullptr);
    m_tracker_client.store(nullptr);
    
    g_logger->info("[{}] Sync Task stopped.", m_task.sync_key);
}


void SyncNode::start() {
    // 防止重复启动
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        g_logger->warn("[{}] SyncNode::start() called multiple times, ignoring.", m_task.sync_key);
        return;
    }
    
    g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
    g_logger->info("[Config] Role: {}", m_task.role);
    g_logger->info("[Config] Sync Folder: {}", m_task.sync_folder);

    // ===== 配置验证 =====
    if (m_task.sync_key.empty()) {
        g_logger->error("[SyncNode] Invalid config: sync_key is empty.");
        m_started = false;  // 重置状态，允许修正后重新启动
        return;
    }

    if (m_task.sync_folder.empty()) {
        g_logger->error("[SyncNode] Invalid config: sync_folder is empty.");
        m_started = false;
        return;
    }

    if (m_task.role != "source" && m_task.role != "destination") {
        g_logger->error("[SyncNode] Invalid role: '{}' (must be 'source' or 'destination').", m_task.role);
        m_started = false;
        return;
    }

    // 1. 使用 EncodingUtils 转换路径（可能抛出异常，外层应捕获）
    std::filesystem::path sync_path;
    try {
        sync_path = Utf8ToPath(m_task.sync_folder);
    } catch (const std::exception& e) {
        g_logger->error("[SyncNode] Failed to convert path '{}': {}", m_task.sync_folder, e.what());
        m_started = false;
        return;
    }


    // 2. 解析角色（已在上面验证过，必定有效）
    SyncRole role = (m_task.role == "source") ? SyncRole::Source : SyncRole::Destination;


    // 开启文件监控的条件
    bool enable_watcher = (role == SyncRole::Source) || (m_task.mode == SyncMode::BiDirectional);

    // 2. 确保目录存在
    std::error_code ec;
    if (!std::filesystem::exists(sync_path, ec)) {
        std::filesystem::create_directories(sync_path, ec);
        if (ec) {
            g_logger->error("[SyncNode] 创建同步目录失败: {}", ec.message());
            m_started = false;  // 重置状态
            return;
        }
        g_logger->info("[SyncNode] 创建同步目录成功");
    } else {
        g_logger->info("[SyncNode] 使用现有同步目录");
    }

    // 3. 创建 P2PManager
    auto p2p = P2PManager::create();
    if (!p2p) {
        g_logger->error("[SyncNode] Failed to create P2PManager.");
        m_started = false;
        return;
    }
    m_p2p_manager.store(p2p);  // 原子存储

    // 4. 创建 TrackerClient
    auto tracker = std::make_shared<TrackerClient>(m_global_config.tracker_host, m_global_config.tracker_port);
    m_tracker_client.store(tracker);  // 原子存储

    // 5. 互相注入依赖
    tracker->set_p2p_manager(p2p.get());
    p2p->set_tracker_client(tracker.get());

    // 6. 配置 P2PManager
    p2p->set_role(role);
    p2p->set_encryption_key(m_task.sync_key);
    p2p->set_mode(m_task.mode);
    
    // 记录关键配置参数
    g_logger->info("[Config] Sync Mode: {}", 
                   m_task.mode == SyncMode::OneWay ? "OneWay" : "BiDirectional");
    g_logger->info("[Config] File Watcher: {}", enable_watcher ? "Enabled" : "Disabled");
    g_logger->info("[Config] Chunk Size: {} bytes", m_global_config.chunk_size);
    g_logger->info("[Config] KCP Window Size: {}", m_global_config.kcp_window_size);



    // 配置 STUN
    if (!m_global_config.stun_host.empty()) {
        g_logger->info("[Config] Using STUN server at {}:{}", m_global_config.stun_host, m_global_config.stun_port);
        p2p->set_stun_config(m_global_config.stun_host, m_global_config.stun_port);
    }

    // 配置 TURN
    if (!m_global_config.turn_host.empty()) {
        g_logger->info("[Config] Using TURN server at {}:{}", m_global_config.turn_host, m_global_config.turn_port);
        p2p->set_turn_config(m_global_config.turn_host, m_global_config.turn_port,
                                       m_global_config.turn_username, m_global_config.turn_password);
    }

    // 7. 创建 StateManager（可能抛出异常，需要捕获）
    try {
        m_state_manager =
            std::make_unique<StateManager>(m_task.sync_folder, *p2p, enable_watcher, m_task.sync_key);
    } catch (const std::exception& e) {
        g_logger->error("[SyncNode] Failed to create StateManager: {}", e.what());
        
        // 清理已创建的资源
        m_p2p_manager.store(nullptr);
        m_tracker_client.store(nullptr);
        m_started = false;
        return;
    }

    // 8. 注入 StateManager
    p2p->set_state_manager(m_state_manager.get());


    // 9. 初始扫描
    m_state_manager->scan_directory();

    // 10. 启动信令连接（使用 weak_ptr 保证生命周期安全）
    g_logger->info("[{}] --- Phase 1: Contacting Tracker ---", m_task.sync_key);

    // 🔥 正确方案：捕获 weak_ptr，回调中检查 SyncNode 是否还存活
    // 这样即使 SyncNode 已销毁，回调也能安全检测并退出
    std::weak_ptr<SyncNode> weak_self = shared_from_this();
    std::string sync_key_copy = m_task.sync_key;

    m_tracker_client.load()->connect(sync_key_copy, [weak_self, sync_key_copy](std::vector<std::string> peer_list) {
        // 🛡️ 尝试锁定 weak_ptr，如果失败说明 SyncNode 已销毁
        auto self = weak_self.lock();
        if (!self) {
            g_logger->warn("[{}] Callback aborted: SyncNode has been destroyed.", sync_key_copy);
            return;
        }

        // 🛡️ 检查是否正在停止
        if (self->m_is_stopping.load(std::memory_order_acquire)) {
            g_logger->warn("[{}] Callback aborted: SyncNode is stopping.", sync_key_copy);
            return;
        }

        // 🛡️ 检查 P2PManager 是否还存在
        auto p2p = self->m_p2p_manager.load();
        if (!p2p) {
            g_logger->warn("[{}] Callback aborted: P2PManager is null.", sync_key_copy);
            return;
        }

        // ✅ 安全执行
        g_logger->info("[{}] --- Phase 2: P2P (ICE) connection ---", sync_key_copy);
        p2p->connect_to_peers(peer_list);
    });
}

}  // namespace VeritasSync