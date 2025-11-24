#include <gtest/gtest.h>

#include <vector>

#include "VeritasSync/SyncManager.h"

using namespace VeritasSync;

// 辅助：创建一个 dummy 回调，模拟永远查不到历史 Hash
auto dummy_history_func = [](const std::string&) -> std::string { return ""; };

TEST(SyncLogicTest, DetectsNewRemoteFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;

    FileInfo f1;
    f1.path = "a.txt";
    f1.hash = "abc";
    f1.modified_time = 100;
    remote.push_back(f1);

    // [修复] 传入 dummy_history_func
    SyncActions actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_history_func);

    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "a.txt");
    EXPECT_EQ(actions.files_to_delete.size(), 0);
}

TEST(SyncLogicTest, DetectsModifiedRemoteFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;

    FileInfo l1;
    l1.path = "a.txt";
    l1.hash = "old_hash";
    local.push_back(l1);

    FileInfo r1;
    r1.path = "a.txt";
    r1.hash = "new_hash";
    remote.push_back(r1);

    // [修复] 传入 dummy_history_func
    SyncActions actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_history_func);

    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "a.txt");
}

TEST(SyncLogicTest, DetectsDeletedFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;

    FileInfo l1;
    l1.path = "deleted.txt";
    l1.hash = "abc";
    local.push_back(l1);

    // [修复] 传入 dummy_history_func，默认模式 OneWay
    SyncActions actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_history_func);

    ASSERT_EQ(actions.files_to_request.size(), 0);
    ASSERT_EQ(actions.files_to_delete.size(), 1);
    EXPECT_EQ(actions.files_to_delete[0], "deleted.txt");
}

TEST(SyncLogicTest, IgnoresIdenticalFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;

    FileInfo f1;
    f1.path = "same.txt";
    f1.hash = "hash123";
    local.push_back(f1);
    remote.push_back(f1);

    // [修复] 传入 dummy_history_func
    SyncActions actions = SyncManager::compare_states_and_get_requests(local, remote, dummy_history_func);

    EXPECT_EQ(actions.files_to_request.size(), 0);
    EXPECT_EQ(actions.files_to_delete.size(), 0);
}

// [新增测试] 双向模式下的智能保留
TEST(SyncLogicTest, BiDirectionalKeepNewFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;

    FileInfo l1;
    l1.path = "new_local.txt";
    l1.hash = "abc";
    local.push_back(l1);

    // [修复] 传入 dummy_history_func (无历史) + BiDirectional 模式
    // 因为无历史，SyncManager 应该认为是新增文件，从而保留它（不删除）
    SyncActions actions =
        SyncManager::compare_states_and_get_requests(local, remote, dummy_history_func, SyncMode::BiDirectional);

    EXPECT_EQ(actions.files_to_delete.size(), 0);  // 应该保留
}