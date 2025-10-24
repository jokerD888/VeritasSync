#include "VeritasSync/SyncManager.h"

#include <iostream>
#include <map>
#include <set>  // 用于辅助

namespace VeritasSync {

SyncActions SyncManager::analyze_states(
    const std::map<std::string, FileInfo>& local_files,  // key 是 UTF-8 string
    const std::map<std::string, FileInfo>& remote_files) {
  SyncActions actions;

  std::cout << "[SyncManager] 正在比较本地状态 (" << local_files.size()
            << " 个条目) 与远程状态 (" << remote_files.size() << " 个条目)."
            << std::endl;

  // 1. 遍历本地文件/目录
  for (const auto& [path, local_info] : local_files) {
    auto it = remote_files.find(path);

    if (it == remote_files.end()) {
      // 情况一: 远程没有此条目。它们需要拉取。
      std::cout << "[SyncManager] -> 远程需要: " << path << std::endl;
      actions.push_to_remote.push_back(path);
    } else {
      // 情况二: 远程也有此条目。
      const auto& remote_info = it->second;

      // 【重要 文件夹修复】 如果两者都是目录，则它们相同
      if (local_info.hash == "DIRECTORY" && remote_info.hash == "DIRECTORY") {
        // 都是目录，什么都不做
      }
      // 检查哈希值是否不同 (现在也包括 文件 vs 目录 的情况)
      else if (local_info.hash != remote_info.hash) {
        // 哈希值不同，这是一个冲突。
        std::cout << "[SyncManager] -> 冲突: " << path
                  << " (L:" << local_info.hash.substr(0, 7)
                  << ", R:" << remote_info.hash.substr(0, 7) << ")"
                  << std::endl;
        actions.conflicts.push_back(path);
      }
      // else: 哈希值相同 (包括都是目录的情况)，什么都不做。
    }
  }

  // 2. 遍历远程文件/目录 (只查找本地没有的)
  for (const auto& [path, remote_info] : remote_files) {
    if (local_files.find(path) == local_files.end()) {
      // 情况三: 本地没有此条目。我们需要拉取。
      std::cout << "[SyncManager] -> 本地需要: " << path << std::endl;
      actions.request_from_remote.push_back(path);
    }
  }

  return actions;
}

}  // namespace VeritasSync