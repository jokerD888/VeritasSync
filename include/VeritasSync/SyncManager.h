#pragma once

#include <string>
#include <vector>

#include "VeritasSync/Protocol.h"  // 需要引入以使用 FileInfo 结构体

namespace VeritasSync {

// 用于封装状态比较结果的结构体
struct SyncActions {
  std::vector<std::string> request_from_remote;  // 我们需要从对方请求的文件
  std::vector<std::string> push_to_remote;       // 对方需要从我们这里请求的文件
  std::vector<std::string> conflicts;            // 双方都已修改的文件
};


class SyncManager {
 public:
  // 比较本地和远程的状态，并确定需要执行的所有同步操作。
  //
  // @param local_files 代表本地目录状态的 FileInfo map。
  // @param remote_files 代表远程目录状态的 FileInfo map。
  // @return 一个 SyncActions 结构体，包含了所有需要执行的操作。
  static SyncActions analyze_states(
      const std::map<std::string, FileInfo>& local_files,
      const std::map<std::string, FileInfo>& remote_files);
};

}  // namespace VeritasSync