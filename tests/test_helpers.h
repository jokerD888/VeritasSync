// tests/test_helpers.h
// 共享测试基础设施：消除 12 处 TestEnvironment 重复 + make_file / cleanup_db_files 重复
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/sync/Protocol.h"

namespace VeritasSync::TestHelpers {

// ═══════════════════════════════════════════════════════════════
// 全局测试环境：仅初始化一次 Logger
// 用法：在每个测试 .cpp 中加上 REGISTER_VERITAS_TEST_ENV();
// ═══════════════════════════════════════════════════════════════
class GlobalTestEnv : public ::testing::Environment {
public:
    void SetUp() override { init_logger(); }
};

// 宏用于在每个 .cpp 的匿名命名空间中注册全局环境
// 每个翻译单元可以各自注册，gtest 保证 SetUp 只执行一次（幂等）
#define REGISTER_VERITAS_TEST_ENV()                                              \
    static ::testing::Environment* const veritas_test_env_ =                     \
        ::testing::AddGlobalTestEnvironment(new VeritasSync::TestHelpers::GlobalTestEnv())

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

/// 快速创建 FileInfo (原来在 test_sync_logic.cpp 和 test_sync_manager_enhanced.cpp 中重复定义)
inline FileInfo make_file(const std::string& path, const std::string& hash, uint64_t mtime = 1000) {
    return {path, hash, mtime};
}

/// 清理 SQLite 数据库及其附属文件（WAL/SHM/Journal），带重试
/// (原来在 test_database.cpp 和 test_database_advanced.cpp 中重复定义)
inline void cleanup_db_files(const std::string& db_path, int max_retries = 3) {
    for (int i = 0; i < max_retries; ++i) {
        std::error_code ec;
        bool any_fail = false;
        
        for (const auto& suffix : {"", "-wal", "-shm", "-journal"}) {
            std::string file = db_path + suffix;
            if (std::filesystem::exists(file)) {
                std::filesystem::remove(file, ec);
                if (ec) any_fail = true;
            }
        }
        
        if (!any_fail) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace VeritasSync::TestHelpers
