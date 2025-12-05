#include <gtest/gtest.h>

#include <optional>  // 引入 optional
#include <vector>

#include "VeritasSync/SyncManager.h"

using namespace VeritasSync;

// 辅助函数：创建 FileInfo
FileInfo make_file(const std::string& path, const std::string& hash, uint64_t mtime = 1000) {
    return {path, hash, mtime};
}

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