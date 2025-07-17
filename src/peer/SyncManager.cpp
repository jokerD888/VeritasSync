#include "VeritasSync/SyncManager.h"

#include <iostream>
#include <map>

namespace VeritasSync {

std::vector<std::string> SyncManager::compare_states_and_get_requests(
    const std::vector<FileInfo>& local_files,
    const std::vector<FileInfo>& remote_files) {
  std::vector<std::string> files_to_request;

  // 为了高效查找，将本地文件列表转换为一个map
  // 键: 文件路径, 值: 文件哈希值
  std::map<std::string, std::string> local_file_hashes;
  for (const auto& info : local_files) {
    local_file_hashes[info.path] = info.hash;
  }

  std::cout << "[SyncManager] 正在比较本地状态 (" << local_files.size()
            << " 个文件) 与远程状态 (" << remote_files.size() << " 个文件)."
            << std::endl;

  // 遍历远程节点拥有的每一个文件
  for (const auto& remote_file : remote_files) {
    auto it = local_file_hashes.find(remote_file.path);

    // 情况一: 本地完全没有这个文件。请求它。
    if (it == local_file_hashes.end()) {
      std::cout << "[SyncManager] -> 需要请求新文件: " << remote_file.path
                << std::endl;
      files_to_request.push_back(remote_file.path);
    }
    // 情况二: 本地有这个文件，但哈希值不同。请求它。
    // (注意: 更复杂的实现可能还会检查修改时间)
    else if (it->second != remote_file.hash) {
      std::cout << "[SyncManager] -> 需要请求更新的文件: " << remote_file.path
                << std::endl;
      files_to_request.push_back(remote_file.path);
    }
    // 情况三: 本地拥有相同版本的文件。什么都不做。
  }

  if (files_to_request.empty()) {
    std::cout << "[SyncManager] 所有文件都已是最新。无需请求。" << std::endl;
  }

  return files_to_request;
}

}  // namespace VeritasSync