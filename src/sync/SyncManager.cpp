#include "VeritasSync/sync/SyncManager.h"

#include <map>
#include <set>
#include <unordered_map>  // 性能优化
#include <unordered_set>  // 性能优化

// 1. 引入 Logger 头文件
#include "VeritasSync/common/Logger.h"
// 2. 引入路径工具（跨平台路径处理）
#include "VeritasSync/common/PathUtils.h"

/*
情况 1：遍历远程文件（决定下载）

远程文件: file.txt (hash: BBB)
           │
           │ 查找本地
           ↓
    ┌─────────────────┐
    │  本地是否存在？  │
    └───┬─────────┬───┘
        │NO       │YES
        ↓         ↓
  ┌──────────┐  ┌──────────────────┐
  │ 下载文件  │  │ 本地hash == BBB？ │
  └──────────┘  └───┬──────────┬───┘
                    │YES       │NO
                    ↓          ↓
              ┌──────────┐  ┌─────────────────┐
              │ 无需操作  │  │ hash不同，需更新 │
              └──────────┘  └────────┬────────┘
                                     │
                        ┌────────────┴────────────┐
                        │    双向模式？           │
                        └───┬──────────────┬──────┘
                            │NO            │YES
                            ↓              ↓
                      ┌──────────┐   ┌──────────────┐
                      │ 下载文件  │   │  冲突检测     │
                      └──────────┘   └──────┬───────┘
                                            │
                         ┌──────────────────┴──────────────────┐
                         │         查询历史记录                 │
                         └──────┬──────────────┬───────────────┘
                                │无记录        │有记录
                                ↓              ↓
                         ┌──────────────┐  ┌───────────────────┐
                         │ 离线新建冲突  │  │ 本地hash == 历史？ │
                         │ → 重命名+下载 │  └───┬───────────┬───┘
                         └──────────────┘      │YES        │NO
                                               ↓           ↓
                                        ┌──────────┐  ┌────────────┐
                                        │只远程改了 │  │ 并发修改    │
                                        │→ 直接下载 │  │→ 重命名+下载│
                                        └──────────┘  └────────────┘
情况 2：遍历本地文件（决定删除）

本地文件: file.txt (hash: AAA)
           │
           │ 查找远程
           ↓
    ┌─────────────────┐
    │  远程是否存在？  │
    └───┬─────────┬───┘
        │YES      │NO
        ↓         ↓
  ┌──────────┐  ┌──────────────┐
  │ 无需操作  │  │  删除判断     │
  └──────────┘  └──────┬───────┘
                       │
              ┌────────┴────────┐
              │   单向模式？     │
              └───┬──────────┬──┘
                  │YES       │NO (双向)
                  ↓          ↓
            ┌──────────┐  ┌────────────────┐
            │ 直接删除  │  │  查询历史记录   │
            └──────────┘  └────────┬───────┘
                                   │
             ┌─────────────────────┼─────────────────────┐
             │无记录                │有记录                │
             ↓                     ↓                     │
       ┌──────────┐         ┌──────────────┐           │
       │ 本地新增  │         │ hash == 历史？│           │
       │ → 保留   │         └───┬──────┬───┘           │
       └──────────┘             │YES   │NO              │
                                ↓      ↓                │
                         ┌──────────┐ ┌──────────┐     │
                         │ 远程删除 │ │ 本地修改  │     │
                         │ → 删除  │ │ → 保留   │     │
                         └──────────┘ └──────────┘     │

*/

namespace VeritasSync {

/**
 * @brief 比较本地和远程状态，决定同步操作
 * 
 * 优化列表：
 * ✅ 路径归一化：使用 PathUtils 解决跨平台大小写问题
 * ✅ 内存优化：unordered_map + reserve 减少内存分配
 * ✅ 异常处理：安全包装 get_history 回调
 * 
 * @param local_files 本地文件列表
 * @param remote_files 远程文件列表
 * @param get_history 历史记录查询回调（可能抛异常）
 * @param mode 同步模式
 * @return 同步操作清单
 */
SyncActions SyncManager::compare_states_and_get_requests(const std::vector<FileInfo>& local_files,
                                                         const std::vector<FileInfo>& remote_files,
                                                         HistoryQueryFunc get_history, SyncMode mode) {
    SyncActions actions;

    // 🛡️ 输入验证
    if (!get_history) {
        g_logger->error("[SyncManager] get_history 回调为空！");
        return actions;  // 返回空结果
    }

    // 🚀 性能优化：预留空间，减少 vector 动态扩容
    actions.files_to_request.reserve(remote_files.size());
    actions.files_to_delete.reserve(local_files.size());
    // 冲突通常较少，预留 10% 容量
    actions.files_to_conflict_rename.reserve(
        std::min(local_files.size(), remote_files.size()) / 10
    );

    // 🔑 安全包装：捕获 get_history 可能抛出的异常
    auto safe_get_history = [&](const std::string& path) -> std::optional<SyncHistory> {
        try {
            return get_history(path);
        } catch (const std::exception& e) {
            g_logger->error("[SyncManager] 查询历史记录失败 ({}): {}", path, e.what());
            return std::nullopt;  // 当作无历史记录处理
        } catch (...) {
            g_logger->error("[SyncManager] 查询历史记录失败 (未知异常): {}", path);
            return std::nullopt;
        }
    };

    // --- 阶段 1：构建查找表 (优化：unordered_map + reserve + 路径归一化) ---
    
    // 🚀 性能优化1：使用 unordered_map (O(1) 查找)
    // 🚀 性能优化2：使用自定义哈希/比较器 (大小写不敏感)
    std::unordered_map<std::string, std::string, 
                       PathUtils::CaseInsensitiveHash,
                       PathUtils::CaseInsensitiveEqual> local_file_hashes;
    
    // 🚀 性能优化3：预留空间，避免重复扩容
    local_file_hashes.reserve(local_files.size());

    for (const auto& info : local_files) {
        // 🌍 跨平台优化：归一化路径
        std::string normalized_path = PathUtils::normalize(info.path);
        local_file_hashes[normalized_path] = info.hash;
    }

    // 远程路径集合（同样使用归一化）
    std::unordered_set<std::string,
                       PathUtils::CaseInsensitiveHash,
                       PathUtils::CaseInsensitiveEqual> remote_file_paths;
    remote_file_paths.reserve(remote_files.size());

    for (const auto& info : remote_files) {
        std::string normalized_path = PathUtils::normalize(info.path);
        remote_file_paths.insert(normalized_path);
    }

    // --- 阶段 2：查找请求文件 (使用归一化路径) ---
    for (const auto& remote_file : remote_files) {
        std::string normalized_path = PathUtils::normalize(remote_file.path);
        auto it = local_file_hashes.find(normalized_path);

        if (it == local_file_hashes.end()) {
            // 本地没有 -> 请求下载
            actions.files_to_request.push_back(remote_file.path);  // 使用原始路径
        } else if (it->second != remote_file.hash) {
            // 本地有，但 Hash 不同。检查是否冲突。
            bool is_conflict = false;

            if (mode == SyncMode::BiDirectional) {
                // 🛡️ 使用安全包装的回调
                auto history_opt = safe_get_history(remote_file.path);
                std::string local_hash = it->second;

                // 🔑 使用提取的冲突检测函数
                is_conflict = detect_conflict(local_hash, remote_file.hash, history_opt);

                // 记录冲突类型（便于调试）
                if (is_conflict) {
                    if (!history_opt.has_value()) {
                        g_logger->warn("[Sync] 离线新建冲突: {}", remote_file.path);
                    } else {
                        g_logger->warn("[Sync] 离线修改冲突: {}", remote_file.path);
                    }
                }
            }

            // 如果判定为冲突，加入重命名列表
            if (is_conflict) {
                actions.files_to_conflict_rename.push_back(remote_file.path);
            }

            // 无论是否冲突，最后都要把远程的最新版下载下来（覆盖原名文件）
            // 区别在于：如果是冲突，上面会先让 P2PManager 把本地文件改名备份。
            actions.files_to_request.push_back(remote_file.path);
        }
    }

    // --- 阶段 3：查找删除文件 ---
    for (const auto& local_file : local_files) {
        std::string normalized_path = PathUtils::normalize(local_file.path);
        
        // 如果本地文件在远程找不到
        if (remote_file_paths.find(normalized_path) == remote_file_paths.end()) {
            bool should_delete = false;

            if (mode == SyncMode::OneWay) {
                should_delete = true;
            } else {
                // [双向模式] 智能判断
                // 🛡️ 使用安全包装的回调
                auto history_opt = safe_get_history(local_file.path);

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

/**
 * @brief 比较本地和远程目录状态
 * 
 * 优化：
 * ✅ 路径归一化：与文件比较保持一致
 * ✅ 使用 unordered_set：O(1) 查找
 * 
 * 注意：执行层（P2PManager）会进行安全检查：
 * - 双向模式：只删除空目录
 * - 单向模式：递归删除
 */
DirSyncActions SyncManager::compare_dir_states(const std::set<std::string>& local_dirs,
                                               const std::set<std::string>& remote_dirs, SyncMode mode) {
    DirSyncActions actions;

    // 🌍 构建归一化的查找集合（大小写不敏感）
    std::unordered_set<std::string,
                       PathUtils::CaseInsensitiveHash,
                       PathUtils::CaseInsensitiveEqual> local_dir_set;
    local_dir_set.reserve(local_dirs.size());

    for (const auto& dir : local_dirs) {
        std::string normalized = PathUtils::normalize(dir);
        local_dir_set.insert(normalized);
    }

    std::unordered_set<std::string,
                       PathUtils::CaseInsensitiveHash,
                       PathUtils::CaseInsensitiveEqual> remote_dir_set;
    remote_dir_set.reserve(remote_dirs.size());

    for (const auto& dir : remote_dirs) {
        std::string normalized = PathUtils::normalize(dir);
        remote_dir_set.insert(normalized);
    }

    // 1. 需创建
    for (const auto& remote_dir : remote_dirs) {
        std::string normalized = PathUtils::normalize(remote_dir);
        if (local_dir_set.find(normalized) == local_dir_set.end()) {
            actions.dirs_to_create.push_back(remote_dir);  // 使用原始路径
        }
    }

    // 2. 需删除
    for (const auto& local_dir : local_dirs) {
        std::string normalized = PathUtils::normalize(local_dir);
        if (remote_dir_set.find(normalized) == remote_dir_set.end()) {
            // 注意：安全性由执行层（P2PManager）保证：
            // - 双向模式：只删除空目录（std::filesystem::remove）
            // - 单向模式：递归删除（std::filesystem::remove_all）
            actions.dirs_to_delete.push_back(local_dir);  // 使用原始路径
        }
    }

    return actions;
}

// ============================================================================
// 私有辅助函数
// ============================================================================

/**
 * @brief 冲突检测函数（三向合并算法）
 * 
 * 算法逻辑：
 * 
 * 前提：已知 local_hash != remote_hash（外层已检查）
 * 
 * 情况1: 无历史记录
 *   → 说明：双方都在离线期间新建了同名文件
 *   → 判定：冲突（无法确定谁是"正确"的）
 * 
 * 情况2: 有历史记录
 *   → 比较 local_hash 与 base_hash：
 *      - 如果 local == base：本地未修改，只是远程更新了
 *        → 判定：不是冲突，直接下载远程版本
 *      - 如果 local != base：本地修改了，且已知远程也不同
 *        → 判定：冲突（基于旧版本的并发修改）
 */
bool SyncManager::detect_conflict(
    const std::string& local_hash,
    const std::string& remote_hash,
    const std::optional<SyncHistory>& history
) {
    // 场景1: 无历史记录 → 离线新建冲突
    if (!history.has_value()) {
        return true;
    }

    // 场景2: 有历史记录
    const std::string& base_hash = history->hash;
    
    // 如果本地修改了（与历史不同），则是冲突
    // 因为外层已知 local != remote，所以远程也修改了
    if (local_hash != base_hash) {
        return true;  // 并发修改冲突
    }

    // 本地未修改（local == base），只是远程更新了
    return false;  // 不是冲突，正常更新
}

}  // namespace VeritasSync