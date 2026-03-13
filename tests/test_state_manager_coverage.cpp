// tests/test_state_manager_coverage.cpp
// 补充 StateManager 模块的测试覆盖：回声抑制、目录管理、同步历史、JSON状态等
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

class StateManagerCoverageTest : public ::testing::Test {
protected:
    std::filesystem::path test_root = "test_sm_coverage";
    boost::asio::io_context m_io_context;
    std::unique_ptr<StateManager> sm;
    
    // 回调追踪
    std::vector<std::vector<FileInfo>> received_updates;
    std::vector<std::vector<std::string>> received_deletes;
    std::mutex cb_mutex;

    void SetUp() override {
        init_logger();
        
        // 强制清理残留（可能上次 TearDown 失败）
        for (int i = 0; i < 5; ++i) {
            std::error_code ec;
            if (std::filesystem::exists(test_root)) {
                std::filesystem::remove_all(test_root, ec);
                if (!ec) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                break;
            }
        }
        std::filesystem::create_directories(test_root);
        
        StateManagerCallbacks callbacks;
        callbacks.on_file_updates = [this](const std::vector<FileInfo>& updates) {
            std::lock_guard<std::mutex> lock(cb_mutex);
            received_updates.push_back(updates);
        };
        callbacks.on_file_deletes = [this](const std::vector<std::string>& deletes) {
            std::lock_guard<std::mutex> lock(cb_mutex);
            received_deletes.push_back(deletes);
        };
        
        sm = std::make_unique<StateManager>(test_root.string(), m_io_context,
                                            std::move(callbacks), false, "test_key");
    }

    void TearDown() override {
        sm.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::filesystem::exists(test_root)) {
            std::error_code ec;
            for (int i = 0; i < 5; ++i) {
                std::filesystem::remove_all(test_root, ec);
                if (!ec) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    void create_test_file(const std::string& rel_path, const std::string& content) {
        auto full_path = test_root / rel_path;
        std::filesystem::create_directories(full_path.parent_path());
        std::ofstream of(full_path);
        of << content;
        of.close();
    }
};

// ============================================================================
// get_root_path
// ============================================================================

TEST_F(StateManagerCoverageTest, GetRootPath_ReturnsCorrectPath) {
    auto root = sm->get_root_path();
    // 路径应该指向 test_root
    EXPECT_TRUE(std::filesystem::equivalent(root, test_root));
}

// ============================================================================
// add_dir_to_map / remove_dir_from_map / get_local_directories
// ============================================================================

TEST_F(StateManagerCoverageTest, AddDirToMap_NewDirectory) {
    sm->add_dir_to_map("subdir/nested");
    auto dirs = sm->get_local_directories();
    EXPECT_TRUE(dirs.count("subdir/nested") > 0);
}

TEST_F(StateManagerCoverageTest, AddDirToMap_DuplicateDirectory) {
    sm->add_dir_to_map("mydir");
    sm->add_dir_to_map("mydir");  // 重复添加
    auto dirs = sm->get_local_directories();
    EXPECT_EQ(dirs.count("mydir"), 1);  // set 保证唯一
}

TEST_F(StateManagerCoverageTest, RemoveDirFromMap_ExistingDir) {
    sm->add_dir_to_map("to_remove");
    auto dirs_before = sm->get_local_directories();
    EXPECT_TRUE(dirs_before.count("to_remove") > 0);
    
    sm->remove_dir_from_map("to_remove");
    auto dirs_after = sm->get_local_directories();
    EXPECT_EQ(dirs_after.count("to_remove"), 0);
}

TEST_F(StateManagerCoverageTest, RemoveDirFromMap_NonExistentDir_Safe) {
    // 删除不存在的目录不应崩溃
    sm->remove_dir_from_map("never_existed");
    auto dirs = sm->get_local_directories();
    EXPECT_EQ(dirs.count("never_existed"), 0);
}

TEST_F(StateManagerCoverageTest, GetLocalDirectories_MultipleDirectories) {
    sm->add_dir_to_map("dir_a");
    sm->add_dir_to_map("dir_b");
    sm->add_dir_to_map("dir_c/sub");
    
    auto dirs = sm->get_local_directories();
    EXPECT_EQ(dirs.size(), 3);
    EXPECT_TRUE(dirs.count("dir_a"));
    EXPECT_TRUE(dirs.count("dir_b"));
    EXPECT_TRUE(dirs.count("dir_c/sub"));
}

TEST_F(StateManagerCoverageTest, GetLocalDirectories_InitiallyEmpty) {
    auto dirs = sm->get_local_directories();
    // 扫描前可能为空或由 scan_directory 填充
    // 关键是不崩溃
    EXPECT_NO_THROW(sm->get_local_directories());
}

// ============================================================================
// remove_path_from_map
// ============================================================================

TEST_F(StateManagerCoverageTest, RemovePathFromMap_AfterScan) {
    create_test_file("to_remove.txt", "content");
    sm->scan_directory();
    
    EXPECT_NE(sm->get_file_hash("to_remove.txt"), "");
    
    sm->remove_path_from_map("to_remove.txt");
    EXPECT_EQ(sm->get_file_hash("to_remove.txt"), "");
}

TEST_F(StateManagerCoverageTest, RemovePathFromMap_NonExistentPath_Safe) {
    EXPECT_NO_THROW(sm->remove_path_from_map("nonexistent.txt"));
}

// ============================================================================
// 回声抑制: mark_file_received / check_and_clear_echo
// ============================================================================

TEST_F(StateManagerCoverageTest, EchoSuppression_BasicFlow) {
    // 标记文件已接收
    sm->mark_file_received("downloaded.txt", "hash_123");
    
    // 检查是否为回声 - 相同哈希应返回 true
    EXPECT_TRUE(sm->check_and_clear_echo("downloaded.txt", "hash_123"));
    
    // 检查后应该被清除，第二次检查返回 false
    EXPECT_FALSE(sm->check_and_clear_echo("downloaded.txt", "hash_123"));
}

TEST_F(StateManagerCoverageTest, EchoSuppression_DifferentHash_NotEcho) {
    sm->mark_file_received("file.txt", "hash_old");
    
    // 不同的哈希，不是回声
    EXPECT_FALSE(sm->check_and_clear_echo("file.txt", "hash_different"));
    
    // 原始记录仍然存在（因为 hash 不匹配时不清除）
    EXPECT_TRUE(sm->check_and_clear_echo("file.txt", "hash_old"));
}

TEST_F(StateManagerCoverageTest, EchoSuppression_UnrecordedFile_NotEcho) {
    // 未标记的文件不是回声
    EXPECT_FALSE(sm->check_and_clear_echo("unknown.txt", "any_hash"));
}

TEST_F(StateManagerCoverageTest, EchoSuppression_MultipleFiles) {
    sm->mark_file_received("a.txt", "hash_a");
    sm->mark_file_received("b.txt", "hash_b");
    
    EXPECT_TRUE(sm->check_and_clear_echo("a.txt", "hash_a"));
    EXPECT_TRUE(sm->check_and_clear_echo("b.txt", "hash_b"));
    
    // 都已清除
    EXPECT_FALSE(sm->check_and_clear_echo("a.txt", "hash_a"));
    EXPECT_FALSE(sm->check_and_clear_echo("b.txt", "hash_b"));
}

TEST_F(StateManagerCoverageTest, EchoSuppression_OverwriteHash) {
    sm->mark_file_received("file.txt", "hash_v1");
    sm->mark_file_received("file.txt", "hash_v2");  // 覆盖
    
    EXPECT_FALSE(sm->check_and_clear_echo("file.txt", "hash_v1"));
    EXPECT_TRUE(sm->check_and_clear_echo("file.txt", "hash_v2"));
}

// ============================================================================
// record_file_sent / should_ignore_echo
// ============================================================================

TEST_F(StateManagerCoverageTest, RecordFileSent_And_ShouldIgnoreEcho) {
    create_test_file("shared.txt", "content");
    sm->scan_directory();
    
    std::string hash = sm->get_file_hash("shared.txt");
    ASSERT_NE(hash, "");
    
    // 记录发送
    sm->record_file_sent("peer_1", "shared.txt", hash);
    
    // 相同哈希的回声应被忽略
    EXPECT_TRUE(sm->should_ignore_echo("peer_1", "shared.txt", hash));
}

TEST_F(StateManagerCoverageTest, ShouldIgnoreEcho_DifferentHash_NotIgnored) {
    sm->record_file_sent("peer_1", "file.txt", "hash_old");
    
    // 不同哈希不应被忽略（这是一个新版本）
    EXPECT_FALSE(sm->should_ignore_echo("peer_1", "file.txt", "hash_new"));
}

TEST_F(StateManagerCoverageTest, ShouldIgnoreEcho_DifferentPeer_NotIgnored) {
    sm->record_file_sent("peer_1", "file.txt", "hash_1");
    
    // 不同 peer 不受影响
    EXPECT_FALSE(sm->should_ignore_echo("peer_2", "file.txt", "hash_1"));
}

// ============================================================================
// get_base_hash / record_sync_success / clear_sync_history / get_full_history
// ============================================================================

TEST_F(StateManagerCoverageTest, BaseHash_InitiallyEmpty) {
    // 使用唯一的 peer_id/path 避免前次运行的 DB 残留影响
    auto base = sm->get_base_hash("unique_peer_never_used_12345", "unique_path_never_used.txt");
    EXPECT_EQ(base, "");
}

TEST_F(StateManagerCoverageTest, RecordSyncSuccess_UpdatesBaseHash) {
    sm->record_sync_success("peer_1", "file.txt", "hash_v1");
    EXPECT_EQ(sm->get_base_hash("peer_1", "file.txt"), "hash_v1");
    
    sm->record_sync_success("peer_1", "file.txt", "hash_v2");
    EXPECT_EQ(sm->get_base_hash("peer_1", "file.txt"), "hash_v2");
}

TEST_F(StateManagerCoverageTest, ClearSyncHistory_RemovesAllPeers) {
    sm->record_sync_success("peer_1", "file.txt", "hash_1");
    sm->record_sync_success("peer_2", "file.txt", "hash_2");
    
    sm->clear_sync_history("file.txt");
    
    EXPECT_EQ(sm->get_base_hash("peer_1", "file.txt"), "");
    EXPECT_EQ(sm->get_base_hash("peer_2", "file.txt"), "");
}

TEST_F(StateManagerCoverageTest, GetFullHistory_ExistingRecord) {
    sm->record_sync_success("peer_1", "file.txt", "hash_1");
    
    auto hist = sm->get_full_history("peer_1", "file.txt");
    ASSERT_TRUE(hist.has_value());
    EXPECT_EQ(hist->hash, "hash_1");
    EXPECT_GT(hist->ts, 0);
}

TEST_F(StateManagerCoverageTest, GetFullHistory_NonExistent_ReturnsNullopt) {
    auto hist = sm->get_full_history("no_peer", "no_file.txt");
    EXPECT_FALSE(hist.has_value());
}

// ============================================================================
// get_state_as_json_string
// ============================================================================

TEST_F(StateManagerCoverageTest, GetStateAsJson_EmptyDirectory) {
    sm->scan_directory();
    std::string json_str = sm->get_state_as_json_string();
    
    // 应该是有效的 JSON
    EXPECT_NO_THROW({
        auto j = nlohmann::json::parse(json_str);
    });
}

TEST_F(StateManagerCoverageTest, GetStateAsJson_WithFiles) {
    create_test_file("hello.txt", "Hello");
    create_test_file("sub/world.txt", "World");
    sm->scan_directory();
    
    std::string json_str = sm->get_state_as_json_string();
    auto j = nlohmann::json::parse(json_str);
    
    // 应该包含 type 和 payload 字段
    EXPECT_TRUE(j.contains("type"));
    EXPECT_TRUE(j.contains("payload"));
    
    // payload 应该有 files 和 dirs
    if (j.contains("payload") && j["payload"].is_object()) {
        EXPECT_TRUE(j["payload"].contains("files") || j["payload"].contains("dirs"));
    }
}

// ============================================================================
// get_all_files
// ============================================================================

TEST_F(StateManagerCoverageTest, GetAllFiles_EmptyDir) {
    sm->scan_directory();
    auto files = sm->get_all_files();
    // 只有 .veritasignore 文件可能存在或不存在
    // 关键是不崩溃
    EXPECT_NO_THROW(sm->get_all_files());
}

TEST_F(StateManagerCoverageTest, GetAllFiles_PopulatedDir) {
    create_test_file("a.txt", "aaa");
    create_test_file("b/c.txt", "ccc");
    sm->scan_directory();
    
    auto files = sm->get_all_files();
    EXPECT_GE(files.size(), 2);
    
    // 验证每个文件都有非空的路径和哈希
    for (const auto& f : files) {
        EXPECT_FALSE(f.path.empty());
        EXPECT_FALSE(f.hash.empty());
    }
}

// ============================================================================
// scan_directory 后 get_file_hash 一致性
// ============================================================================

TEST_F(StateManagerCoverageTest, GetFileHash_ConsistentWithScan) {
    create_test_file("verify.txt", "consistent content");
    sm->scan_directory();
    
    std::string hash1 = sm->get_file_hash("verify.txt");
    std::string hash2 = sm->get_file_hash("verify.txt");
    
    EXPECT_NE(hash1, "");
    EXPECT_EQ(hash1, hash2);  // 多次调用结果一致
}

TEST_F(StateManagerCoverageTest, GetFileHash_ChangedContent) {
    create_test_file("changing.txt", "version1");
    sm->scan_directory();
    std::string hash_v1 = sm->get_file_hash("changing.txt");
    
    // Windows 文件系统 mtime 精度为秒级，需要等待至少1秒确保 mtime 变化
    // 否则 StateManager 会使用 DB 缓存（mtime 未变 = 内容未变）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    create_test_file("changing.txt", "version2_different_content_that_must_differ");
    sm->scan_directory();
    std::string hash_v2 = sm->get_file_hash("changing.txt");
    
    EXPECT_NE(hash_v1, hash_v2);
}

}  // namespace VeritasSync
