// tests/test_logger_coverage.cpp
// 补充 Logger 模块的测试覆盖：set_log_level 所有分支
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/sink.h>

#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

class LoggerCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_logger();
    }
    
    void TearDown() override {
        // 恢复默认日志级别
        set_log_level("info");
    }
};

// ============================================================================
// set_log_level 正常分支
// ============================================================================

TEST_F(LoggerCoverageTest, SetLogLevel_Debug) {
    EXPECT_NO_THROW(set_log_level("debug"));
    EXPECT_EQ(g_logger->level(), spdlog::level::debug);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Info) {
    set_log_level("debug");  // 先改成别的
    EXPECT_NO_THROW(set_log_level("info"));
    EXPECT_EQ(g_logger->level(), spdlog::level::info);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Warn) {
    EXPECT_NO_THROW(set_log_level("warn"));
    EXPECT_EQ(g_logger->level(), spdlog::level::warn);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Warning_Alias) {
    EXPECT_NO_THROW(set_log_level("warning"));
    EXPECT_EQ(g_logger->level(), spdlog::level::warn);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Error) {
    EXPECT_NO_THROW(set_log_level("error"));
    EXPECT_EQ(g_logger->level(), spdlog::level::err);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Err_Alias) {
    EXPECT_NO_THROW(set_log_level("err"));
    EXPECT_EQ(g_logger->level(), spdlog::level::err);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Critical) {
    EXPECT_NO_THROW(set_log_level("critical"));
    EXPECT_EQ(g_logger->level(), spdlog::level::critical);
}

TEST_F(LoggerCoverageTest, SetLogLevel_Off) {
    EXPECT_NO_THROW(set_log_level("off"));
    EXPECT_EQ(g_logger->level(), spdlog::level::off);
}

// ============================================================================
// set_log_level 无效输入
// ============================================================================

TEST_F(LoggerCoverageTest, SetLogLevel_Invalid_KeepsCurrentLevel) {
    set_log_level("debug");
    auto level_before = g_logger->level();
    
    set_log_level("bogus_level");
    // 无效级别不应改变当前级别
    EXPECT_EQ(g_logger->level(), level_before);
}

TEST_F(LoggerCoverageTest, SetLogLevel_EmptyString_KeepsCurrentLevel) {
    set_log_level("warn");
    auto level_before = g_logger->level();
    
    set_log_level("");
    EXPECT_EQ(g_logger->level(), level_before);
}

TEST_F(LoggerCoverageTest, SetLogLevel_CaseSensitive) {
    // 大写应该被认为是无效的（map 使用小写 key）
    set_log_level("info");
    auto level_before = g_logger->level();
    
    set_log_level("DEBUG");  // 大写
    // 如果 map 中没有 "DEBUG"，级别不应该改变
    EXPECT_EQ(g_logger->level(), level_before);
}

// ============================================================================
// set_log_level 同步所有 sink 的级别
// ============================================================================

TEST_F(LoggerCoverageTest, SetLogLevel_SyncsSinkLevels) {
    set_log_level("debug");
    
    // 验证所有 sink 的级别都被同步更新
    for (const auto& sink : g_logger->sinks()) {
        EXPECT_EQ(sink->level(), spdlog::level::debug);
    }
    
    set_log_level("error");
    for (const auto& sink : g_logger->sinks()) {
        EXPECT_EQ(sink->level(), spdlog::level::err);
    }
}

// ============================================================================
// init_logger 幂等性
// ============================================================================

TEST_F(LoggerCoverageTest, InitLogger_Idempotent) {
    // 多次调用 init_logger 不应崩溃
    init_logger();
    init_logger();
    init_logger();
    
    EXPECT_NE(g_logger, nullptr);
}
