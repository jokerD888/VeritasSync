// tests/test_database_advanced.cpp
// Database 写操作已从 void 改为 bool 返回值
#include "test_helpers.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <thread>

#include "VeritasSync/storage/Database.h"

using namespace VeritasSync;

class DatabaseAdvancedTest : public ::testing::Test {
protected:
    std::string db_path = "test_adv_veritas.db";
    std::unique_ptr<Database> db;

    void SetUp() override {
        TestHelpers::cleanup_db_files(db_path);
        db = std::make_unique<Database>(db_path);
    }

    void TearDown() override {
        db.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestHelpers::cleanup_db_files(db_path);
    }
};

// 1. 测试 TransactionGuard 自动回滚 (RAII)
TEST_F(DatabaseAdvancedTest, TransactionRAIIRollback) {
    {
        Database::TransactionGuard guard(*db);
        db->update_file("tx_test.txt", "initial_hash", 1000);
        // 不显式提交，离开作用域。
    }

    // 验证文件是否未被写入
    EXPECT_FALSE(db->get_file("tx_test.txt").has_value());
}

// 2. 测试 TransactionGuard 显式提交
TEST_F(DatabaseAdvancedTest, TransactionCommit) {
    {
        Database::TransactionGuard guard(*db);
        db->update_file("committed.txt", "secure_hash", 2000);
        guard.commit(); // 提交
    }

    auto meta = db->get_file("committed.txt");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->hash, "secure_hash");
}

// 3. 测试 递归事务 (recursive_mutex 支持)
TEST_F(DatabaseAdvancedTest, RecursiveTransactions) {
    {
        Database::TransactionGuard outer(*db);
        db->update_file("outer.txt", "outer", 1);
        
        {
            Database::TransactionGuard inner(*db);
            db->update_file("inner.txt", "inner", 2);
            inner.commit();
        }
        
        // 虽然内部提交了，但由于 SQLite 事务是原子的，
        // 在大多数实现中，嵌套事务通常由外层控制，或者 SQLite 忽略嵌套 begin。
        // 因为我们使用的是 recursive_mutex，这里主要测试不产生死锁。
        outer.commit();
    }
    
    EXPECT_TRUE(db->get_file("outer.txt").has_value());
    EXPECT_TRUE(db->get_file("inner.txt").has_value());
}

// 4. 测试 异常/错误回滚场景
TEST_F(DatabaseAdvancedTest, RollbackOnError) {
    db->update_file("pre_existing.txt", "old", 0);
    
    try {
        Database::TransactionGuard guard(*db);
        db->update_file("pre_existing.txt", "new_attempt", 999);
        
        // 模拟异常发生
        throw std::runtime_error("simulated failure");
        
        guard.commit();
    } catch (...) {
        // 异常被捕获，guard 被自动销毁并回滚
    }
    
    auto meta = db->get_file("pre_existing.txt");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->hash, "old"); // 应该是旧值
}

// 5. 测试 状态句柄重用与并发稳定性
TEST_F(DatabaseAdvancedTest, StatementLeakAndStress) {
    const int COUNT = 1000;
    Database::TransactionGuard guard(*db);
    for (int i = 0; i < COUNT; ++i) {
        db->update_file("file_" + std::to_string(i), "hash", i);
    }
    guard.commit();
    
    EXPECT_EQ(db->get_all_file_paths().size(), COUNT);
}
