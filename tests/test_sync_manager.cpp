#include <gtest/gtest.h>

#include "VeritasSync/Protocol.h"
#include "VeritasSync/SyncManager.h"

using namespace VeritasSync;

// 定义一个辅助函数来创建 FileInfo
FileInfo CreateInfo(std::string path, std::string hash,
                    uint64_t mtime = 12345) {
  return FileInfo{path, hash, mtime};
}

// 定义我们的测试夹具 (Test Fixture)
class SyncManagerTest : public ::testing::Test {
 protected:
  std::map<std::string, FileInfo> local;
  std::map<std::string, FileInfo> remote;
};

// 测试场景 1: 完全同步
TEST_F(SyncManagerTest, FullySynced) {
  local["file.txt"] = CreateInfo("file.txt", "hash_A");
  remote["file.txt"] = CreateInfo("file.txt", "hash_A");

  SyncActions actions = SyncManager::analyze_states(local, remote);

  ASSERT_TRUE(actions.request_from_remote.empty());
  ASSERT_TRUE(actions.push_to_remote.empty());
  ASSERT_TRUE(actions.conflicts.empty());
}

// 测试场景 2: 本地需要拉取 (远程有新文件)
TEST_F(SyncManagerTest, ShouldRequest) {
  // 本地为空
  remote["new_file.txt"] = CreateInfo("new_file.txt", "hash_new");

  SyncActions actions = SyncManager::analyze_states(local, remote);

  ASSERT_EQ(actions.request_from_remote.size(), 1);
  ASSERT_EQ(actions.request_from_remote[0], "new_file.txt");
  ASSERT_TRUE(actions.push_to_remote.empty());
  ASSERT_TRUE(actions.conflicts.empty());
}

// 测试场景 3: 远程需要拉取 (本地有新文件)
TEST_F(SyncManagerTest, ShouldPush) {
  local["local_file.txt"] = CreateInfo("local_file.txt", "hash_local");
  // 远程为空

  SyncActions actions = SyncManager::analyze_states(local, remote);

  ASSERT_TRUE(actions.request_from_remote.empty());
  ASSERT_EQ(actions.push_to_remote.size(), 1);
  ASSERT_EQ(actions.push_to_remote[0], "local_file.txt");
  ASSERT_TRUE(actions.conflicts.empty());
}

// 测试场景 4: 冲突 (双方都修改了)
TEST_F(SyncManagerTest, ShouldConflict) {
  local["common.txt"] = CreateInfo("common.txt", "hash_LOCAL");
  remote["common.txt"] = CreateInfo("common.txt", "hash_REMOTE");

  SyncActions actions = SyncManager::analyze_states(local, remote);

  ASSERT_TRUE(actions.request_from_remote.empty());
  ASSERT_TRUE(actions.push_to_remote.empty());
  ASSERT_EQ(actions.conflicts.size(), 1);
  ASSERT_EQ(actions.conflicts[0], "common.txt");
}

// 测试场景 5: 复杂混合场景
TEST_F(SyncManagerTest, ComplexScenario) {
  // 1. 同步的文件
  local["synced.txt"] = CreateInfo("synced.txt", "hash_sync");
  remote["synced.txt"] = CreateInfo("synced.txt", "hash_sync");

  // 2. 本地需要拉取
  remote["remote_only.txt"] = CreateInfo("remote_only.txt", "hash_R");

  // 3. 远程需要拉取
  local["local_only.txt"] = CreateInfo("local_only.txt", "hash_L");

  // 4. 冲突
  local["conflict.txt"] = CreateInfo("conflict.txt", "hash_L_v2");
  remote["conflict.txt"] = CreateInfo("conflict.txt", "hash_R_v2");

  SyncActions actions = SyncManager::analyze_states(local, remote);

  // 验证拉取
  ASSERT_EQ(actions.request_from_remote.size(), 1);
  ASSERT_EQ(actions.request_from_remote[0], "remote_only.txt");

  // 验证推送
  ASSERT_EQ(actions.push_to_remote.size(), 1);
  ASSERT_EQ(actions.push_to_remote[0], "local_only.txt");

  // 验证冲突
  ASSERT_EQ(actions.conflicts.size(), 1);
  ASSERT_EQ(actions.conflicts[0], "conflict.txt");
}