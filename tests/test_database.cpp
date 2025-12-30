// tests/test_database.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <thread>

#include "VeritasSync/storage/Database.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

class DatabaseTest : public ::testing::Test {
protected:
    std::string db_path = "test_veritas_unit.db";
    std::unique_ptr<Database> db; // 使用指针以便手动控制释放顺序

    void SetUp() override {
        // 如果文件残留（比如上次测试崩溃了），先强制清理
        cleanup_files();
        db = std::make_unique<Database>(db_path);
    }

    void TearDown() override {
        // 1. 显式断开数据库连接，释放所有 ScopedStmt
        db.reset();

        // 2. 在 Windows 上，给文件系统一点点缓冲时间（10ms 足够）
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 3. 安全清理
        cleanup_files();
    }

private:
    void cleanup_files() {
        std::error_code ec;
        if (std::filesystem::exists(db_path)) {
            std::filesystem::remove(db_path, ec);
        }
        // 同时清理 WAL 产生的临时文件，这些也会导致锁定
        std::filesystem::remove(db_path + "-wal", ec);
        std::filesystem::remove(db_path + "-shm", ec);
        std::filesystem::remove(db_path + "-journal", ec);
    }
};

// 测试 Sync History 的增删改查
TEST_F(DatabaseTest, SyncHistoryUpsert) {
    std::string peer_id = "peer_001";
    std::string file_path = "src/main.cpp";
    std::string hash_v1 = "hash_v1_aabbcc";
    std::string hash_v2 = "hash_v2_ddeeff";

    EXPECT_EQ(db->get_last_sent_hash(peer_id, file_path), "");

    db->update_sync_history(peer_id, file_path, hash_v1);
    EXPECT_EQ(db->get_last_sent_hash(peer_id, file_path), hash_v1);

    db->update_sync_history(peer_id, file_path, hash_v2);
    EXPECT_EQ(db->get_last_sent_hash(peer_id, file_path), hash_v2);

    EXPECT_EQ(db->get_last_sent_hash("peer_002", file_path), "");
}

// 测试基础文件元数据表
TEST_F(DatabaseTest, FileMetadataOperations) {
    db->update_file("test.txt", "abc", 123456);
    auto meta = db->get_file("test.txt");

    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->hash, "abc");
    EXPECT_EQ(meta->mtime, 123456);

    db->remove_file("test.txt");
    EXPECT_FALSE(db->get_file("test.txt").has_value());
}