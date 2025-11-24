#include "VeritasSync/SyncManager.h"

#include <map>
#include <set>

// 1. 引入 Logger 头文件
#include "VeritasSync/Logger.h"

namespace VeritasSync {
SyncActions SyncManager::compare_states_and_get_requests(const std::vector<FileInfo>& local_files,
                                                         const std::vector<FileInfo>& remote_files,
                                                         HistoryQueryFunc get_history_hash,  // 接收回调
                                                         SyncMode mode) {
    SyncActions actions;

    // --- 阶段 1：构建查找表 ---
    std::map<std::string, std::string> local_file_hashes;
    for (const auto& info : local_files) {
        local_file_hashes[info.path] = info.hash;
    }

    std::set<std::string> remote_file_paths;
    for (const auto& info : remote_files) {
        remote_file_paths.insert(info.path);
    }

    g_logger->info("[SyncManager] 比较中... 本地: {}, 远程: {}, 模式: {}", local_files.size(), remote_files.size(),
                   (mode == SyncMode::OneWay ? "OneWay" : "BiDirectional"));

    // --- 阶段 2：查找需要“请求”的文件 (远程有，本地没有或不同) ---
    for (const auto& remote_file : remote_files) {
        auto it = local_file_hashes.find(remote_file.path);
        if (it == local_file_hashes.end()) {
            // 本地没有 -> 请求
            actions.files_to_request.push_back(remote_file.path);
        } else if (it->second != remote_file.hash) {
            // 哈希不同 -> 请求
            actions.files_to_request.push_back(remote_file.path);
        }
    }

    // --- 阶段 3：查找需要“删除”的文件 (本地有，远程没有) ---
    for (const auto& local_file : local_files) {
        // 如果本地文件在远程找不到
        if (remote_file_paths.find(local_file.path) == remote_file_paths.end()) {
            bool should_delete = false;

            if (mode == SyncMode::OneWay) {
                // [单向模式] 简单镜像：远程没有我就删
                should_delete = true;
            } else {
                // [双向模式] 智能判断逻辑
                // 查询数据库：上次同步时，这个文件是什么样子的？
                std::string last_synced_hash = get_history_hash(local_file.path);

                if (last_synced_hash.empty()) {
                    // 情况 A: 没历史记录 -> 说明这是我离线期间新创建的文件 -> 【保留】
                    g_logger->info("[Sync] 双向同步：保留本地新增文件 {}", local_file.path);
                    should_delete = false;
                } else if (last_synced_hash == local_file.hash) {
                    // 情况 B: 历史记录和现在一样 -> 说明我没改过，是对方删了 -> 【跟随删除】
                    g_logger->info("[Sync] 双向同步：检测到远程删除，执行本地删除 {}", local_file.path);
                    should_delete = true;
                } else {
                    // 情况 C: 历史记录不同 -> 说明对方删了，但我改了 -> 【冲突保留】
                    g_logger->warn("[Sync] ⚠️ 双向同步冲突：远程已删除，但本地已修改。保留 {}", local_file.path);
                    should_delete = false;
                }
            }

            if (should_delete) {
                actions.files_to_delete.push_back(local_file.path);
            }
        }
    }

    return actions;
}

DirSyncActions SyncManager::compare_dir_states(const std::set<std::string>& local_dirs,
                                               const std::set<std::string>& remote_dirs, SyncMode mode) {
    DirSyncActions actions;

    // 1. 需创建 (保持不变)
    for (const auto& remote_dir : remote_dirs) {
        if (local_dirs.find(remote_dir) == local_dirs.end()) {
            actions.dirs_to_create.push_back(remote_dir);
        }
    }

    // 2. 需删除
    for (const auto& local_dir : local_dirs) {
        if (remote_dirs.find(local_dir) == remote_dirs.end()) {
            // [修改] 无论单向还是双向，都加入删除计划。
            // 安全性由执行层的 "Empty Check" 保证。
            actions.dirs_to_delete.push_back(local_dir);
        }
    }

    return actions;
}

}  // namespace VeritasSync