#include "test_helpers.h"
#include <gtest/gtest.h>

#include <optional>  // 引入 optional
#include <vector>

#include "VeritasSync/sync/SyncManager.h"

using namespace VeritasSync;
using namespace VeritasSync::TestHelpers;

REGISTER_VERITAS_TEST_ENV();

// 【关键修改】Mock 回调：始终返回“无历史记录”
// 旧代码可能是返回 string，现在必须返回 optional<SyncHistory>
auto dummy_no_history = [](const std::string& path) -> std::optional<SyncHistory> { return std::nullopt; };

TEST(SyncLogicTest, OneWay_NewRemoteFile_ShouldRequest) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {make_file("a.txt", "hash_a")};

    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);

    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "a.txt");
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncLogicTest, OneWay_LocalExtraFile_ShouldDelete) {
    std::vector<FileInfo> local = {make_file("extra.txt", "hash_e")};
    std::vector<FileInfo> remote;

    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);

    ASSERT_EQ(actions.files_to_delete.size(), 1);
    EXPECT_EQ(actions.files_to_delete[0], "extra.txt");
    EXPECT_TRUE(actions.files_to_request.empty());
}

TEST(SyncLogicTest, BiDi_LocalNewFile_NoHistory_ShouldKeep) {
    // 本地新增文件（无历史记录），远程没有
    // 预期：保留（不删除）
    std::vector<FileInfo> local = {make_file("new_local.txt", "hash_new")};
    std::vector<FileInfo> remote;

    auto actions =
        SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::BiDirectional);

    EXPECT_TRUE(actions.files_to_delete.empty());  // 应该保留
}

TEST(SyncLogicTest, BiDi_RemoteDelete_HistoryMatches_ShouldDelete) {
    // 远程删除了（本地有，远程无，历史记录匹配）
    // 预期：删除
    std::vector<FileInfo> local = {make_file("deleted_remote.txt", "hash_old")};
    std::vector<FileInfo> remote;

    // 【关键修改】Mock 回调：返回匹配的历史记录
    auto history_match = [](const std::string& path) -> std::optional<SyncHistory> {
        return SyncHistory{"hash_old", 0};  // 返回匹配的 Hash
    };

    auto actions = SyncManager::compare_states_and_get_requests(local, remote, history_match, SyncMode::BiDirectional);

    ASSERT_EQ(actions.files_to_delete.size(), 1);
    EXPECT_EQ(actions.files_to_delete[0], "deleted_remote.txt");
}

TEST(SyncLogicTest, BiDi_Conflict_HistoryMismatch_ShouldKeep) {
    // 冲突：远程删了，但我改了（历史 Hash 不匹配）
    // 预期：保留
    std::vector<FileInfo> local = {make_file("conflict.txt", "hash_modified")};
    std::vector<FileInfo> remote;

    // 【关键修改】Mock 回调：返回旧的历史记录
    auto history_mismatch = [](const std::string& path) -> std::optional<SyncHistory> {
        return SyncHistory{"hash_original", 0};  // Hash 不匹配
    };

    auto actions =
        SyncManager::compare_states_and_get_requests(local, remote, history_mismatch, SyncMode::BiDirectional);

    EXPECT_TRUE(actions.files_to_delete.empty());  // 应该保留
}

// 【新增测试】连接时间戳判定法测试
// 模拟：历史记录非常新（刚刚产生），被判定为状态滞后 -> 应该保留
TEST(SyncLogicTest, BiDi_FreshHistory_ShouldKeep) {
    std::vector<FileInfo> local = {make_file("just_sent.txt", "hash_x")};
    std::vector<FileInfo> remote;  // 远程还没有这个文件

    // 这里我们在 Test 中不需要模拟时间判断，因为那个逻辑是在 P2PManager 的回调里做的
    // SyncManager 接收到的回调应该已经处理好了“如果是新记录则返回 nullopt”
    // 所以这里的测试用例其实等同于 "NoHistory_ShouldKeep"

    // 如果你想在 SyncManager 内部测试时间逻辑（假设你把时间判断移进去了），写法会不同。
    // 但根据我们最新的设计，时间判断是在 P2PManager 传入的 lambda 里做的。
    // 所以 SyncManager 只负责：如果 get_history 返回 nullopt，就保留。

    // 模拟 P2PManager 的行为：因为记录太新，所以返回 nullopt
    auto history_func_intercepted = [](const std::string& path) -> std::optional<SyncHistory> { return std::nullopt; };

    auto actions =
        SyncManager::compare_states_and_get_requests(local, remote, history_func_intercepted, SyncMode::BiDirectional);

    EXPECT_TRUE(actions.files_to_delete.empty());
}

// ============ 新增测试用例 ============

// --- 边界情况测试 ---

TEST(SyncLogicTest, EmptyBothSides) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncLogicTest, SameFileSameHash) {
    // 本地和远程有相同的文件，相同的 Hash
    std::vector<FileInfo> local = {make_file("same.txt", "hash_same")};
    std::vector<FileInfo> remote = {make_file("same.txt", "hash_same")};
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    // 不需要任何操作
    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncLogicTest, SameFileDifferentHash) {
    // 本地和远程有相同的文件，但 Hash 不同
    std::vector<FileInfo> local = {make_file("updated.txt", "hash_old")};
    std::vector<FileInfo> remote = {make_file("updated.txt", "hash_new")};
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    // 需要请求更新
    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "updated.txt");
}

TEST(SyncLogicTest, MultipleFilesComplex) {
    // 复杂场景：多个文件，混合情况
    std::vector<FileInfo> local = {
        make_file("unchanged.txt", "hash_a"),
        make_file("updated.txt", "hash_old"),
        make_file("local_only.txt", "hash_c"),
    };
    std::vector<FileInfo> remote = {
        make_file("unchanged.txt", "hash_a"),
        make_file("updated.txt", "hash_new"),
        make_file("remote_only.txt", "hash_d"),
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    // 应该请求 updated.txt 和 remote_only.txt
    EXPECT_EQ(actions.files_to_request.size(), 2);
    
    // 应该删除 local_only.txt
    EXPECT_EQ(actions.files_to_delete.size(), 1);
    EXPECT_EQ(actions.files_to_delete[0], "local_only.txt");
}

// --- 多语言路径测试 (使用 ASCII 模拟) ---

TEST(SyncLogicTest, SpecialFilePaths) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {
        make_file("chinese_file.txt", "hash_cn"),
        make_file("japanese_file.txt", "hash_jp"),
        make_file("korean_file.txt", "hash_kr"),
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    EXPECT_EQ(actions.files_to_request.size(), 3);
}

// --- 目录同步测试 ---

TEST(SyncLogicTest, DirSync_RemoteNewDirs_ShouldCreate) {
    std::set<std::string> local_dirs;
    std::set<std::string> remote_dirs = {"dir1", "dir2/subdir"};
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::OneWay);
    
    EXPECT_EQ(actions.dirs_to_create.size(), 2);
    EXPECT_TRUE(actions.dirs_to_delete.empty());
}

TEST(SyncLogicTest, DirSync_LocalExtraDirs_ShouldDelete) {
    std::set<std::string> local_dirs = {"local_dir", "another_dir"};
    std::set<std::string> remote_dirs;
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::OneWay);
    
    EXPECT_TRUE(actions.dirs_to_create.empty());
    EXPECT_EQ(actions.dirs_to_delete.size(), 2);
}

TEST(SyncLogicTest, DirSync_BiDi_BothSidesMarked) {
    std::set<std::string> local_dirs = {"local_new"};
    std::set<std::string> remote_dirs = {"remote_new"};
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::BiDirectional);
    
    // 远程有而本地没有的目录应该被创建
    EXPECT_EQ(actions.dirs_to_create.size(), 1);
    EXPECT_EQ(actions.dirs_to_create[0], "remote_new");
    
    // 实现中：无论单向还是双向，本地多余目录都会被标记为删除
    // 安全性由执行层的 "Empty Check" 保证
    EXPECT_EQ(actions.dirs_to_delete.size(), 1);
    EXPECT_EQ(actions.dirs_to_delete[0], "local_new");
}

// --- 大规模数据测试 ---

TEST(SyncLogicTest, LargeFileCount) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;
    
    // 创建 1000 个文件
    for (int i = 0; i < 1000; ++i) {
        remote.push_back(make_file("file_" + std::to_string(i) + ".txt", "hash_" + std::to_string(i)));
    }
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    EXPECT_EQ(actions.files_to_request.size(), 1000);
}

// --- 特殊字符路径测试 ---

TEST(SyncLogicTest, SpecialCharacterPaths) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {
        make_file("file with spaces.txt", "hash1"),
        make_file("file-with-dashes.txt", "hash2"),
        make_file("file_with_underscores.txt", "hash3"),
        make_file("file.multiple.dots.txt", "hash4"),
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_no_history, SyncMode::OneWay);
    
    EXPECT_EQ(actions.files_to_request.size(), 4);
}