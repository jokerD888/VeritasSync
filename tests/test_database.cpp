// tests/test_database.cpp
#include <gtest/gtest.h>

#include <filesystem>

#include "VeritasSync/Database.h"

using namespace VeritasSync;

class DatabaseTest : public ::testing::Test {
protected:
    std::string db_path = "test_veritas_unit.db";

    void SetUp() override {
        // 确保测试前环境干净
        if (std::filesystem::exists(db_path)) {
            std::filesystem::remove(db_path);
        }
    }

    void TearDown() override {
        // 测试后清理
        if (std::filesystem::exists(db_path)) {
            std::filesystem::remove(db_path);
        }
    }
};

// 测试 Sync History 的增删改查
TEST_F(DatabaseTest, SyncHistoryUpsert) {
    Database db(db_path);

    std::string peer_id = "peer_001";
    std::string file_path = "src/main.cpp";
    std::string hash_v1 = "hash_v1_aabbcc";
    std::string hash_v2 = "hash_v2_ddeeff";

    // 1. 初始状态应为空
    EXPECT_EQ(db.get_last_sent_hash(peer_id, file_path), "");

    // 2. 首次写入 (Insert)
    db.update_sync_history(peer_id, file_path, hash_v1);
    EXPECT_EQ(db.get_last_sent_hash(peer_id, file_path), hash_v1);

    // 3. 更新记录 (Update)
    db.update_sync_history(peer_id, file_path, hash_v2);
    EXPECT_EQ(db.get_last_sent_hash(peer_id, file_path), hash_v2);

    // 4. 测试不同 Peer 隔离
    EXPECT_EQ(db.get_last_sent_hash("peer_002", file_path), "");

    // 5. 测试不同文件隔离
    EXPECT_EQ(db.get_last_sent_hash(peer_id, "src/other.cpp"), "");
}

// 测试基础文件元数据表 (确保没改坏旧功能)
TEST_F(DatabaseTest, FileMetadataOperations) {
    Database db(db_path);

    db.update_file("test.txt", "abc", 123456);
    auto meta = db.get_file("test.txt");

    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->hash, "abc");
    EXPECT_EQ(meta->mtime, 123456);

    db.remove_file("test.txt");
    EXPECT_FALSE(db.get_file("test.txt").has_value());
}