#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/storage/Database.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync {
    /**
     * @brief 同步操作清单（文件）
     * 
     * ⚠️ 重要：执行顺序约定
     * 
     * P2PManager 必须按以下顺序执行，否则可能导致文件系统操作失败：
     * 
     * 1. 创建目录 (DirSyncActions::dirs_to_create)
     *    → 确保文件的父目录存在
     * 
     * 2. 冲突重命名 (files_to_conflict_rename)
     *    → 保护本地修改的副本
     * 
     * 3. 下载/更新文件 (files_to_request)
     *    → 填充文件内容
     * 
     * 4. 删除文件 (files_to_delete)
     *    → 清理废弃文件
     * 
     * 5. 删除目录 (DirSyncActions::dirs_to_delete)
     *    → 最后删除空目录
     * 
     * 当前实现：P2PManager::handle_share_state() 已按此顺序执行 ✅
     */
    struct SyncActions {
        std::vector<std::string> files_to_request;  // 需要下载/更新的文件
        std::vector<std::string> files_to_delete;   // 本地多余，需要删除的文件
        std::vector<std::string> files_to_conflict_rename;  // 需要执行冲突重命名的文件列表
    };

    /**
     * @brief 同步操作清单（目录）
     * 
     * ⚠️ 执行顺序：见 SyncActions 文档
     */
    struct DirSyncActions {
        std::vector<std::string> dirs_to_create;  // 需要创建的目录
        std::vector<std::string> dirs_to_delete;  // 需要删除的目录
    };

    class SyncManager {
    public:
        using HistoryQueryFunc = std::function<std::optional<SyncHistory>(const std::string& path)>;

        // 比较本地和远程的状态，并确定需要执行的同步操作。
        // @return 一个 SyncActions 结构体，包含要请求的和要删除的文件列表。

        static SyncActions compare_states_and_get_requests(const std::vector<FileInfo>& local_files,
                                                           const std::vector<FileInfo>& remote_files,
                                                           HistoryQueryFunc get_history,
                                                           SyncMode mode = SyncMode::OneWay);

        static DirSyncActions compare_dir_states(const std::set<std::string>& local_dirs,
                                                 const std::set<std::string>& remote_dirs,
                                                 SyncMode mode = SyncMode::OneWay);

    private:
        /**
         * @brief 检测文件冲突（三向合并）
         * 
         * 冲突定义：
         * - 场景1: 无历史记录 → 双方都离线新建了同名文件
         * - 场景2: 本地修改了（local != base）且 远程也不同（已知 local != remote）
         * 
         * @param local_hash 本地文件哈希
         * @param remote_hash 远程文件哈希
         * @param history 历史记录（可能为空）
         * @return true 如果是冲突，false 如果不是冲突
         */
        static bool detect_conflict(
            const std::string& local_hash,
            const std::string& remote_hash,
            const std::optional<SyncHistory>& history
        );
    };

}  // namespace VeritasSync
