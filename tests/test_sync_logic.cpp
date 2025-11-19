#include <gtest/gtest.h>
#include <spdlog/sinks/stdout_color_sinks.h>  // [新增] 需要包含 spdlog 头文件
#include <spdlog/spdlog.h>

#include "VeritasSync/Logger.h"
#include "VeritasSync/Protocol.h"
#include "VeritasSync/SyncManager.h"

namespace VeritasSync {
std::shared_ptr<spdlog::logger> g_logger = []() {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("test_logger", console);
    logger->set_level(spdlog::level::debug);
    return logger;
}();
}  // namespace VeritasSync

using namespace VeritasSync;

// --- 文件同步逻辑测试 ---

// 1. 基础场景：远程有新文件 -> 应请求下载
TEST(SyncLogicTest, RequestMissingFile) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {{"new.txt", "hash_abc", 100}};

    auto actions = SyncManager::compare_states_and_get_requests(local, remote);

    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "new.txt");
}

// 2. 基础场景：本地有多余文件 -> 应删除
TEST(SyncLogicTest, DeleteExtraFile) {
    std::vector<FileInfo> local = {{"trash.txt", "hash_x", 100}};
    std::vector<FileInfo> remote;

    auto actions = SyncManager::compare_states_and_get_requests(local, remote);

    ASSERT_EQ(actions.files_to_delete.size(), 1);
    EXPECT_EQ(actions.files_to_delete[0], "trash.txt");
}

// 3. 基础场景：内容变化 -> 应更新
TEST(SyncLogicTest, UpdateChangedFile) {
    std::vector<FileInfo> local = {{"config.ini", "hash_v1", 100}};
    std::vector<FileInfo> remote = {{"config.ini", "hash_v2", 200}};  // 哈希不同

    auto actions = SyncManager::compare_states_and_get_requests(local, remote);

    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "config.ini");
}

// 4. 边界场景：完全一致 -> 无操作
TEST(SyncLogicTest, NoActionWhenIdentical) {
    std::vector<FileInfo> local = {{"data.bin", "hash_same", 500}};
    std::vector<FileInfo> remote = {{"data.bin", "hash_same", 500}};

    auto actions = SyncManager::compare_states_and_get_requests(local, remote);

    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
}

// 5. 目录同步测试：创建与删除
TEST(SyncLogicTest, DirectorySync) {
    std::set<std::string> local_dirs = {"old_dir"};
    std::set<std::string> remote_dirs = {"new_dir"};

    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs);

    // 应该删除 old_dir
    ASSERT_EQ(actions.dirs_to_delete.size(), 1);
    EXPECT_EQ(actions.dirs_to_delete[0], "old_dir");

    // 应该创建 new_dir
    ASSERT_EQ(actions.dirs_to_create.size(), 1);
    EXPECT_EQ(actions.dirs_to_create[0], "new_dir");
}

