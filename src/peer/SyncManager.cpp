#include "VeritasSync/SyncManager.h"

#include <map>
#include <set>

// 1. 引入 Logger 头文件
#include "VeritasSync/Logger.h"

namespace VeritasSync {
SyncActions SyncManager::compare_states_and_get_requests(const std::vector<FileInfo>& local_files,
                                                         const std::vector<FileInfo>& remote_files) {
    SyncActions actions;

    // --- 阶段 1：构建查找表 ---

    // 本地文件 Map (路径 -> 哈希)
    std::map<std::string, std::string> local_file_hashes;
    for (const auto& info : local_files) {
        local_file_hashes[info.path] = info.hash;
    }

    // 远程文件 Set (路径)
    std::set<std::string> remote_file_paths;
    for (const auto& info : remote_files) {
        remote_file_paths.insert(info.path);
    }

       g_logger->info("[SyncManager] 正在比较本地状态 ({} 个文件) 与远程状态 ({} 个文件).", local_files.size(),
                   remote_files.size());

    // --- 阶段 2：查找需要“请求”的文件 (遍历远程列表) ---
    for (const auto& remote_file : remote_files) {
        auto it = local_file_hashes.find(remote_file.path);

        // 情况一: 本地完全没有这个文件。请求它。
        if (it == local_file_hashes.end()) {
            g_logger->info("[SyncManager] -> 需要请求新文件: {}", remote_file.path);
            actions.files_to_request.push_back(remote_file.path);
        }
        // 情况二: 本地有这个文件，但哈希值不同。请求它。
        else if (it->second != remote_file.hash) {
            g_logger->info("[SyncManager] -> 需要请求更新的文件: {}", remote_file.path);
            actions.files_to_request.push_back(remote_file.path);
        }
        // 情况三: 本地拥有相同版本的文件。什么都不做。
    }

    // --- 阶段 3：查找需要“删除”的文件 (遍历本地列表) ---
    for (const auto& local_file : local_files) {
        // 如果本地文件在远程文件列表中找不到，说明它应该被删除。
        if (remote_file_paths.find(local_file.path) == remote_file_paths.end()) {
            g_logger->info("[SyncManager] -> 需要删除多余文件: {}", local_file.path);
            actions.files_to_delete.push_back(local_file.path);
        }
    }

    if (actions.files_to_request.empty() && actions.files_to_delete.empty()) {
        g_logger->info("[SyncManager] 所有文件都已是最新。无需操作。");
    }

    return actions;
}

DirSyncActions SyncManager::compare_dir_states(const std::set<std::string>& local_dirs,
                                               const std::set<std::string>& remote_dirs) {
    DirSyncActions actions;

    // 查找需要创建的目录 (存在于远程，但本地没有)
    for (const auto& remote_dir : remote_dirs) {
        if (local_dirs.find(remote_dir) == local_dirs.end()) {
            actions.dirs_to_create.push_back(remote_dir);
        }
    }

    // 查找需要删除的目录 (存在于本地，但远程没有)
    for (const auto& local_dir : local_dirs) {
        if (remote_dirs.find(local_dir) == remote_dirs.end()) {
            actions.dirs_to_delete.push_back(local_dir);
        }
    }

    return actions;
}

}  // namespace VeritasSync