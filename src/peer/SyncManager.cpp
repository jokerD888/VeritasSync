#include "VeritasSync/SyncManager.h"

#include <map>
#include <set>

// 1. 引入 Logger 头文件
#include "VeritasSync/Logger.h"

namespace VeritasSync {
SyncActions SyncManager::compare_states_and_get_requests(const std::vector<FileInfo>& local_files,
                                                         const std::vector<FileInfo>& remote_files,
                                                         HistoryQueryFunc get_history, SyncMode mode) {
    SyncActions actions;

    // --- 阶段 1：构建查找表 (保持不变) ---
    std::map<std::string, std::string> local_file_hashes;
    for (const auto& info : local_files) {
        local_file_hashes[info.path] = info.hash;
    }

    std::set<std::string> remote_file_paths;
    for (const auto& info : remote_files) {
        remote_file_paths.insert(info.path);
    }

    // --- 阶段 2：查找请求文件 (保持不变) ---
    for (const auto& remote_file : remote_files) {
        auto it = local_file_hashes.find(remote_file.path);
        if (it == local_file_hashes.end()) {
            actions.files_to_request.push_back(remote_file.path);
        } else if (it->second != remote_file.hash) {
            actions.files_to_request.push_back(remote_file.path);
        }
    }

    // --- 阶段 3：查找删除文件 (【核心修改部分】) ---
    for (const auto& local_file : local_files) {
        // 如果本地文件在远程找不到
        if (remote_file_paths.find(local_file.path) == remote_file_paths.end()) {
            bool should_delete = false;

            if (mode == SyncMode::OneWay) {
                should_delete = true;
            } else {
                // [双向模式] 智能判断
                // 调用回调获取历史记录对象
                auto history_opt = get_history(local_file.path);

                if (!history_opt.has_value()) {
                    // 情况 A: 没历史记录 (或者被上层拦截屏蔽了) -> 本地新增 -> 【保留】
                    g_logger->info("[Sync] 双向同步：保留本地新增文件 {}", local_file.path);
                    should_delete = false;
                } else {
                    const auto& history = history_opt.value();

                    // 这里不需要再判断时间了，时间过滤将在 P2PManager 的回调中完成
                    // 这里只负责纯粹的 Hash 对比
                    if (history.hash == local_file.hash) {
                        // 情况 B: 历史 Hash 匹配 -> 远程真实删除 -> 【跟随删除】
                        g_logger->info("[Sync] 双向同步：检测到远程删除，执行本地删除 {}", local_file.path);
                        should_delete = true;
                    } else {
                        // 情况 C: 冲突 (Hash 不变但历史存在? 其实这里主要指内容变了) -> 【保留】
                        g_logger->warn("[Sync] ⚠️ 双向同步冲突或修改：保留 {}", local_file.path);
                        should_delete = false;
                    }
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