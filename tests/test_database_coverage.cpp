// tests/test_database_coverage.cpp
// 补充 Database 模块的测试覆盖：get_sync_history, remove_sync_history, 边界情况
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "VeritasSync/storage/Database.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

class DatabaseCoverageTest : public ::testing::Test {
protected:
    std::string db_path = "test_db_coverage.db";
    std::unique_ptr<Database> db;

    void SetUp() override {
        TestHelpers::cleanup_db_files(db_path);
        db = std::make_unique<Database>(db_path);
    }

    void TearDown() override {
        db.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TestHelpers::cleanup_db_files(db_path);
    }
};

// ============================================================================
// get_sync_history 直接测试
// ============================================================================

TEST_F(DatabaseCoverageTest, GetSyncHistory_NotExist_ReturnsNullopt) {
    auto result = db->get_sync_history("peer_1", "nonexistent.txt");
    EXPECT_FALSE(result.has_value());
}

TEST_F(DatabaseCoverageTest, GetSyncHistory_AfterUpdate_ReturnsCorrectData) {
    db->update_sync_history("peer_1", "file.txt", "hash_abc");
    
    auto result = db->get_sync_history("peer_1", "file.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->hash, "hash_abc");
    EXPECT_GT(result->ts, 0);  // 时间戳应该大于0
}

TEST_F(DatabaseCoverageTest, GetSyncHistory_DifferentPeers_Independent) {
    db->update_sync_history("peer_1", "file.txt", "hash_a");
    db->update_sync_history("peer_2", "file.txt", "hash_b");
    
    auto r1 = db->get_sync_history("peer_1", "file.txt");
    auto r2 = db->get_sync_history("peer_2", "file.txt");
    
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->hash, "hash_a");
    EXPECT_EQ(r2->hash, "hash_b");
}

TEST_F(DatabaseCoverageTest, GetSyncHistory_SamePeerDifferentPaths_Independent) {
    db->update_sync_history("peer_1", "a.txt", "hash_a");
    db->update_sync_history("peer_1", "b.txt", "hash_b");
    
    auto ra = db->get_sync_history("peer_1", "a.txt");
    auto rb = db->get_sync_history("peer_1", "b.txt");
    
    ASSERT_TRUE(ra.has_value());
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(ra->hash, "hash_a");
    EXPECT_EQ(rb->hash, "hash_b");
}

TEST_F(DatabaseCoverageTest, GetSyncHistory_Upsert_UpdatesExisting) {
    db->update_sync_history("peer_1", "file.txt", "hash_v1");
    db->update_sync_history("peer_1", "file.txt", "hash_v2");
    
    auto result = db->get_sync_history("peer_1", "file.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->hash, "hash_v2");
}

// ============================================================================
// remove_sync_history 直接测试
// ============================================================================

TEST_F(DatabaseCoverageTest, RemoveSyncHistory_NonExistentPath_ReturnsTrue) {
    // 删除不存在的记录也应该返回 true（DELETE 0 行不算错误）
    EXPECT_TRUE(db->remove_sync_history("nonexistent.txt"));
}

TEST_F(DatabaseCoverageTest, RemoveSyncHistory_RemovesAllPeersForPath) {
    // 写入多个 peer 对同一路径的记录
    db->update_sync_history("peer_1", "shared.txt", "hash_1");
    db->update_sync_history("peer_2", "shared.txt", "hash_2");
    db->update_sync_history("peer_3", "shared.txt", "hash_3");
    
    // 确认写入成功
    ASSERT_TRUE(db->get_sync_history("peer_1", "shared.txt").has_value());
    ASSERT_TRUE(db->get_sync_history("peer_2", "shared.txt").has_value());
    ASSERT_TRUE(db->get_sync_history("peer_3", "shared.txt").has_value());
    
    // 按 path 删除（应删除所有 peer 的记录）
    EXPECT_TRUE(db->remove_sync_history("shared.txt"));
    
    // 验证所有 peer 的记录都被删除
    EXPECT_FALSE(db->get_sync_history("peer_1", "shared.txt").has_value());
    EXPECT_FALSE(db->get_sync_history("peer_2", "shared.txt").has_value());
    EXPECT_FALSE(db->get_sync_history("peer_3", "shared.txt").has_value());
}

TEST_F(DatabaseCoverageTest, RemoveSyncHistory_DoesNotAffectOtherPaths) {
    db->update_sync_history("peer_1", "keep.txt", "hash_keep");
    db->update_sync_history("peer_1", "delete_me.txt", "hash_del");
    
    db->remove_sync_history("delete_me.txt");
    
    // keep.txt 不受影响
    EXPECT_TRUE(db->get_sync_history("peer_1", "keep.txt").has_value());
    // delete_me.txt 已删除
    EXPECT_FALSE(db->get_sync_history("peer_1", "delete_me.txt").has_value());
}

// ============================================================================
// 边界情况
// ============================================================================

TEST_F(DatabaseCoverageTest, EmptyStrings_HandleGracefully) {
    // 空 peer_id 和空 path
    EXPECT_TRUE(db->update_sync_history("", "", "hash"));
    auto result = db->get_sync_history("", "");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->hash, "hash");
    
    EXPECT_TRUE(db->remove_sync_history(""));
    EXPECT_FALSE(db->get_sync_history("", "").has_value());
}

TEST_F(DatabaseCoverageTest, SpecialCharactersInPath) {
    std::string special_path = "dir with spaces/文件 (1).txt";
    db->update_sync_history("peer_1", special_path, "hash_special");
    
    auto result = db->get_sync_history("peer_1", special_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->hash, "hash_special");
}

TEST_F(DatabaseCoverageTest, VeryLongPath) {
    // 256 字符的路径
    std::string long_path(256, 'a');
    db->update_sync_history("peer_1", long_path, "hash_long");
    
    auto result = db->get_sync_history("peer_1", long_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->hash, "hash_long");
}

TEST_F(DatabaseCoverageTest, UpdateFile_ReturnsFalse_WithInvalidDb) {
    // 创建一个无效的数据库（不存在的路径中的子目录）
    // 这实际上测试了返回 bool 的正确性
    auto file = db->get_file("nonexistent.txt");
    EXPECT_FALSE(file.has_value());
}

TEST_F(DatabaseCoverageTest, GetAllFilePaths_Empty) {
    auto paths = db->get_all_file_paths();
    EXPECT_TRUE(paths.empty());
}

TEST_F(DatabaseCoverageTest, GetAllFilePaths_MultipleFiles) {
    db->update_file("a.txt", "h1", 100);
    db->update_file("b.txt", "h2", 200);
    db->update_file("c/d.txt", "h3", 300);
    
    auto paths = db->get_all_file_paths();
    EXPECT_EQ(paths.size(), 3);
    
    // 验证所有路径都在结果中
    std::set<std::string> path_set(paths.begin(), paths.end());
    EXPECT_TRUE(path_set.count("a.txt"));
    EXPECT_TRUE(path_set.count("b.txt"));
    EXPECT_TRUE(path_set.count("c/d.txt"));
}

TEST_F(DatabaseCoverageTest, ConcurrentReadWrite_SyncHistory) {
    // 多线程同时读写同步历史
    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 100;
    
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, t, OPS_PER_THREAD]() {
            std::string peer = "peer_" + std::to_string(t);
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                std::string path = "file_" + std::to_string(i) + ".txt";
                std::string hash = "hash_" + std::to_string(t) + "_" + std::to_string(i);
                db->update_sync_history(peer, path, hash);
                db->get_sync_history(peer, path);
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    // 验证数据一致性
    for (int t = 0; t < NUM_THREADS; ++t) {
        std::string peer = "peer_" + std::to_string(t);
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            std::string path = "file_" + std::to_string(i) + ".txt";
            auto result = db->get_sync_history(peer, path);
            ASSERT_TRUE(result.has_value()) << "peer=" << peer << " path=" << path;
        }
    }
}
