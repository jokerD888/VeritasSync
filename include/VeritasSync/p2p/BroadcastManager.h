#pragma once

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
class StateManager;

/// 广播管理器 — 负责增量广播、全量推送、批量广播、流控和 Anti-Entropy 对账
/// 从 P2PManager 提取，使 P2PManager 聚焦于连接管理和生命周期编排
class BroadcastManager {
public:
    /// 回调类型
    /// send_fn: 广播消息给所有已连接对等点（等价于 P2PManager::send_over_kcp）
    using SendCallback = std::function<bool(const std::string& msg)>;
    /// send_to_peer: 发送消息给指定对等点
    using SendToPeerFunc = std::function<void(const std::string& msg, PeerController* peer)>;

    BroadcastManager(boost::asio::io_context& io_context,
                     boost::asio::thread_pool& worker_pool,
                     PeerRegistry& peer_registry,
                     StateManager* state_manager,
                     SendCallback send_fn,
                     SendToPeerFunc send_to_peer);

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

    // --- 全量推送（flood sync）---
    /// 扫描本地全量文件/目录，批量推送给指定对端
    /// 由连接建立和 Anti-Entropy 对账触发
    void perform_flood_sync(std::shared_ptr<PeerController> controller, uint64_t session_id);

    // --- 流控发送 ---
    void send_to_peers_with_flow_control(
        const std::vector<std::shared_ptr<PeerController>>& peers,
        const std::string& packet);

    // --- Anti-Entropy 对账 ---
    void schedule_reconciliation();

    // --- 生命周期 ---
    void stop();

private:
    bool can_broadcast() const;

    // 异步背压：通过 KCP drain 回调驱动发送节奏
    void pace_and_send_file_batches(
        std::shared_ptr<PeerController> controller,
        uint64_t session_id,
        std::vector<FileInfo> files,
        size_t batch_index);

    // 依赖
    boost::asio::io_context& m_io_context;
    boost::asio::thread_pool& m_worker_pool;
    PeerRegistry& m_peer_registry;
    StateManager* m_state_manager;
    SendCallback m_send_fn;
    SendToPeerFunc m_send_to_peer;

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
