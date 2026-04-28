#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include "VeritasSync/common/Config.h"   // SyncRole, SyncMode
#include "VeritasSync/sync/Protocol.h"   // FileInfo, Protocol constants
#include "VeritasSync/p2p/PeerRegistry.h" // PeerRegistry

namespace VeritasSync {

class PeerController;

/// 广播管理器 — 负责增量广播、批量广播、流控和 Anti-Entropy 对账
/// 从 P2PManager 提取，使 P2PManager 聚焦于连接管理和生命周期编排
class BroadcastManager {
public:
    /// 回调类型
    /// send_fn: 广播消息给所有已连接对等点（等价于 P2PManager::send_over_kcp）
    using SendCallback = std::function<bool(const std::string& msg)>;
    /// flood_sync: 对账时调用 SyncSession::perform_flood_sync
    using FloodSyncCallback = std::function<void(std::shared_ptr<PeerController>, uint64_t session_id)>;
    /// state_provider: 获取当前目录状态 JSON（调用 StateManager）
    using StateProvider = std::function<std::string()>;

    BroadcastManager(boost::asio::io_context& io_context,
                     boost::asio::thread_pool& worker_pool,
                     PeerRegistry& peer_registry,
                     SendCallback send_fn,
                     FloodSyncCallback flood_sync_fn,
                     StateProvider state_provider);

    void set_role(SyncRole role);
    void set_mode(SyncMode mode);

    // --- 单条广播 ---
    void broadcast_current_state();
    void broadcast_file_update(const FileInfo& file_info);
    void broadcast_file_delete(const std::string& relative_path);
    void broadcast_dir_create(const std::string& relative_path);
    void broadcast_dir_delete(const std::string& relative_path);

    // --- 批量广播 ---
    void broadcast_file_updates_batch(const std::vector<FileInfo>& files);
    void broadcast_file_deletes_batch(const std::vector<std::string>& paths);
    void broadcast_dir_changes_batch(const std::vector<std::string>& creates,
                                     const std::vector<std::string>& deletes);

    // --- 优雅关闭 ---
    void broadcast_goodbye();

    // --- 流控发送 ---
    bool send_to_peers_with_flow_control(
        const std::vector<std::shared_ptr<PeerController>>& peers,
        const std::string& packet);

    // --- Anti-Entropy 对账 ---
    void schedule_reconciliation();

    // --- 生命周期 ---
    void stop();

private:
    bool can_broadcast() const;

    // 依赖
    boost::asio::io_context& m_io_context;
    boost::asio::thread_pool& m_worker_pool;
    PeerRegistry& m_peer_registry;
    SendCallback m_send_fn;
    FloodSyncCallback m_flood_sync_fn;
    StateProvider m_state_provider;

    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // Anti-Entropy 对账
    // 增量广播是 Gossip（快但不保证可靠），对账是 Anti-Entropy（保证最终一致）。
    // 每次增量广播后重置定时器，静默 N 秒后自动触发 flood sync 补全。
    static constexpr int RECONCILIATION_DELAY_SECONDS = 30;
    std::unique_ptr<boost::asio::steady_timer> m_reconciliation_timer;
    void trigger_reconciliation();
};

}  // namespace VeritasSync
