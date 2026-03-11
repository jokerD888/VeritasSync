#pragma once

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {

class StateManager;
class PeerController;

/**
 * @brief SyncSession - 同步会话管理器
 * 
 * 从 P2PManager 中提取的同步会话协调逻辑，负责：
 * - perform_flood_sync: 全量推送文件/目录状态给对端
 * - send_sync_begin / handle_sync_begin: 同步会话开始握手
 * - send_sync_ack / handle_sync_ack: 同步会话确认（含重试逻辑）
 * 
 * 设计原则：
 * - 不直接持有网络资源（peers 映射、锁等）
 * - 通过回调函数与 P2PManager 交互
 * - 可独立测试
 */
class SyncSession {
public:
    /**
     * @brief 发送消息给指定 PeerController 的回调
     */
    using SendToPeerFunc = std::function<void(const std::string& msg, PeerController* peer)>;
    
    /**
     * @brief 通过 peer_id 安全查找 peer 并执行操作的回调
     * 在 peers 锁内查找 peer，若连接有效则执行 action
     */
    using WithPeerFunc = std::function<void(const std::string& peer_id,
                                            std::function<void(PeerController*)> action)>;

    /**
     * @brief 通过 peer_id 获取 PeerController shared_ptr 的回调
     * 用于 perform_flood_sync 需要持有 controller 的场景
     */
    using GetPeerFunc = std::function<std::shared_ptr<PeerController>(const std::string& peer_id)>;

    SyncSession(StateManager* state_manager,
                boost::asio::thread_pool& worker_pool,
                boost::asio::io_context& io_context,
                SendToPeerFunc send_to_peer,
                WithPeerFunc with_peer,
                GetPeerFunc get_peer);

    void set_state_manager(StateManager* sm) { m_state_manager = sm; }
    void set_role(SyncRole role) { m_role = role; }
    void set_mode(SyncMode mode) { m_mode = mode; }

    // --- 同步会话管理 ---
    
    /**
     * @brief 向对端发送 sync_begin 消息，通知预期的文件/目录数量
     */
    void send_sync_begin(PeerController* peer, uint64_t session_id, 
                         size_t file_count, size_t dir_count);
    
    /**
     * @brief 向对端发送 sync_ack 确认消息
     */
    void send_sync_ack(PeerController* peer, uint64_t session_id,
                       size_t received_files, size_t received_dirs);
    
    /**
     * @brief 处理收到的 sync_begin 消息
     */
    void handle_sync_begin(const nlohmann::json& payload, PeerController* from_peer);
    
    /**
     * @brief 处理收到的 sync_ack 确认消息
     */
    void handle_sync_ack(const nlohmann::json& payload, PeerController* from_peer);
    
    /**
     * @brief 执行全量同步推送
     * 扫描本地文件/目录，批量推送给指定对端
     */
    void perform_flood_sync(std::shared_ptr<PeerController> controller, uint64_t session_id);

private:
    StateManager* m_state_manager;
    boost::asio::thread_pool& m_worker_pool;
    boost::asio::io_context& m_io_context;

    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;

    // 回调函数
    SendToPeerFunc m_send_to_peer;
    WithPeerFunc m_with_peer;
    GetPeerFunc m_get_peer;

    // 批量大小配置
    static constexpr size_t FILE_UPDATE_BATCH_SIZE = 50;
};

}  // namespace VeritasSync
