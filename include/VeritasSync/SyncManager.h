#pragma once

#include <string>
#include <vector>
#include <set>
#include "VeritasSync/Protocol.h"  // 需要引入以使用 FileInfo 结构体

namespace VeritasSync {
    struct SyncActions {
        std::vector<std::string> files_to_request;  // 需要下载/更新的文件
        std::vector<std::string> files_to_delete;   // 本地多余，需要删除的文件
    };

    struct DirSyncActions {
        std::vector<std::string> dirs_to_create;  // 需要创建的目录
        std::vector<std::string> dirs_to_delete;  // 需要删除的目录
    };

    class SyncManager {
    public:
        // 比较本地和远程的状态，并确定需要执行的同步操作。
        // @return 一个 SyncActions 结构体，包含要请求的和要删除的文件列表。
        static SyncActions compare_states_and_get_requests(
            const std::vector<FileInfo>& local_files,
            const std::vector<FileInfo>& remote_files);

        static DirSyncActions compare_dir_states(
            const std::set<std::string>& local_dirs,
            const std::set<std::string>& remote_dirs);
    };

}  // namespace VeritasSync
