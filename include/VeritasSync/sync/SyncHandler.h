#pragma once

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/sync/SyncManager.h"
#include "VeritasSync/sync/TransferManager.h"

namespace VeritasSync {

class StateManager;
class PeerController;

/**
 * @brief SyncHandler - 同步消息处理器
 * 
 * 从 P2PManager 中提取的纯业务逻辑层，负责处理各类同步消息：
 * - handle_share_state: 全量状态比较与同步
 * - handle_file_update/delete: 增量文件更新/删除
 * - handle_dir_create/delete: 增量目录创建/删除
 * - handle_*_batch: 批量版本
 * 
 * 设计原则：
 * - 不直接持有网络资源（peers 映射、锁等）
 * - 通过回调函数与 P2PManager 交互（发送消息、查找 peer）
 * - 可独立测试
 */
class SyncHandler {
public:
    /**
     * @brief 发送消息回调类型
     * @param msg JSON 字符串
     * @param peer_ctrl 目标 PeerController 指针
     */
    using SendToPeerFunc = std::function<void(const std::string& msg, PeerController* peer)>;
    
    /**
     * @brief 通过 peer_id 安全发送消息的回调
     * @param msg JSON 字符串
     * @param peer_id 目标 peer ID
     */
    using SendToPeerSafeFunc = std::function<void(const std::string& msg, const std::string& peer_id)>;

    /**
     * @brief 查找 peer 并执行操作的回调
     * 在 io_context 线程中执行，持有 peers 读锁
     * @param peer_id 目标 peer ID  
     * @param action 对找到的 PeerController 执行的操作
     */
    using WithPeerFunc = std::function<void(const std::string& peer_id,
                                            std::function<void(PeerController*)> action)>;

    SyncHandler(StateManager* state_manager,
                std::shared_ptr<TransferManager> transfer_manager,
                boost::asio::thread_pool& worker_pool,
                boost::asio::io_context& io_context,
                SendToPeerFunc send_to_peer,
                SendToPeerSafeFunc send_to_peer_safe,
                WithPeerFunc with_peer,
                int sync_timeout_seconds = 60);

    void set_role(SyncRole role) { m_role = role; }
    void set_mode(SyncMode mode) { m_mode = mode; }
    void set_state_manager(StateManager* sm) { m_state_manager = sm; }

    // --- 消息处理器 ---
    void handle_share_state(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_update(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_delete(const nlohmann::json& payload, PeerController* from_peer);
    void handle_dir_create(const nlohmann::json& payload, PeerController* from_peer);
    void handle_dir_delete(const nlohmann::json& payload, PeerController* from_peer);

    // --- 批量消息处理器 ---
    void handle_file_update_batch(const nlohmann::json& payload, PeerController* from_peer);
    void handle_file_delete_batch(const nlohmann::json& payload, PeerController* from_peer);
    void handle_dir_batch(const nlohmann::json& payload, PeerController* from_peer);

private:
    // 刷新 peer 的同步超时定时器
    void refresh_peer_timeout(PeerController* from_peer);

    // 检查是否可以接收同步消息
    bool can_receive() const;

    // 处理单个文件更新（echo check → 冲突检测 → 返回待请求的 FileInfo）
    // 返回 nullopt 表示跳过（echo/冲突无需下载）
    std::optional<FileInfo> process_single_file(const std::string& peer_id,
                                                const FileInfo& remote_info);

    // 异步背压发送：通过 KCP drain 回调驱动发送节奏
    void pace_and_send_files(const std::string& peer_id,
                             std::vector<FileInfo> files_to_request,
                             size_t index = 0);

    // handle_share_state 辅助：同步目录操作（删除多余、创建缺失）
    void sync_directory_actions(const DirSyncActions& dir_actions,
                                SyncMode mode);

    // handle_share_state 辅助：发送文件请求
    void send_file_requests(const std::string& peer_id,
                            const std::vector<std::string>& files_to_request,
                            const std::vector<FileInfo>& remote_files);

    // 提取的辅助函数：安全地从 JSON 解析字段
    template<typename T>
    std::optional<T> get_json_field(const nlohmann::json& payload, 
                                    const std::string& field,
                                    const char* context);

    // 提取的辅助函数：验证路径安全并记录错误
    bool validate_path_safe(const std::filesystem::path& root, 
                            const std::string& rel_path, 
                            const char* context,
                            const char* operation);

    /**
     * @brief 三方合并冲突检测
     * 
     * 比较 local_hash / remote_hash / base_hash，返回结果：
     *  - 1: 应该下载远程文件 (should_request = true)
     *  - 0: 不需要操作 (should_request = false)
     * -1: 跳过当前文件（已一致或本地更新已记录）
     * 
     * 当双方都修改时，自动将本地文件重命名为 .conflict.{timestamp} 并返回 1。
     */
    enum class ConflictResult { RequestRemote, Skip, NoAction };
    ConflictResult resolve_conflict(const std::string& peer_id,
                                    const FileInfo& remote_info,
                                    const std::filesystem::path& full_path,
                                    const std::filesystem::path& relative_path);

    /**
     * @brief 构造文件请求消息（含断点续传检查）
     * @return JSON 字符串，可直接发送给 peer
     */
    std::string build_file_request(const std::string& file_path,
                                   const std::string& peer_id,
                                   const std::string& remote_hash,
                                   uint64_t remote_size);

    StateManager* m_state_manager;
    std::shared_ptr<TransferManager> m_transfer_manager;
    boost::asio::thread_pool& m_worker_pool;
    boost::asio::io_context& m_io_context;

    SyncRole m_role = SyncRole::Source;
    SyncMode m_mode = SyncMode::OneWay;
    int m_sync_timeout_seconds = 60;  // 从 Config::Sync 注入

    // 回调函数
    SendToPeerFunc m_send_to_peer;
    SendToPeerSafeFunc m_send_to_peer_safe;
    WithPeerFunc m_with_peer;
};

}  // namespace VeritasSync
