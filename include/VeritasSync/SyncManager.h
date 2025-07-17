#pragma once

#include <string>
#include <vector>

#include "VeritasSync/Protocol.h"  // 需要引入以使用 FileInfo 结构体

namespace VeritasSync {

class SyncManager {
 public:
  // 比较本地和远程的状态，并确定本地客户端需要向远程节点请求哪些文件。
  //
  // @param local_files 代表本地目录状态的 FileInfo 向量。
  // @param remote_files 代表远程目录状态的 FileInfo 向量。
  // @return 一个字符串向量，每个字符串都是一个需要请求的文件的相对路径。
  static std::vector<std::string> compare_states_and_get_requests(
      const std::vector<FileInfo>& local_files,
      const std::vector<FileInfo>& remote_files);
};

}  // namespace VeritasSync