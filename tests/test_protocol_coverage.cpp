// tests/test_protocol_coverage.cpp
// 补充 Protocol 模块的测试覆盖：FileInfo 比较操作符、size 兼容性、协议常量
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "VeritasSync/sync/Protocol.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

// ============================================================================
// FileInfo::operator==
// ============================================================================

TEST(ProtocolCoverage, FileInfo_EqualOperator_AllFieldsMatch) {
    FileInfo a{"path/file.txt", "hash_abc", 1000, 2048};
    FileInfo b{"path/file.txt", "hash_abc", 1000, 2048};
    EXPECT_TRUE(a == b);
}

TEST(ProtocolCoverage, FileInfo_EqualOperator_DifferentPath) {
    FileInfo a{"path/a.txt", "hash_abc", 1000, 2048};
    FileInfo b{"path/b.txt", "hash_abc", 1000, 2048};
    EXPECT_FALSE(a == b);
}

TEST(ProtocolCoverage, FileInfo_EqualOperator_DifferentHash) {
    FileInfo a{"file.txt", "hash_1", 1000, 2048};
    FileInfo b{"file.txt", "hash_2", 1000, 2048};
    EXPECT_FALSE(a == b);
}

TEST(ProtocolCoverage, FileInfo_EqualOperator_DifferentMtime) {
    FileInfo a{"file.txt", "hash", 1000, 2048};
    FileInfo b{"file.txt", "hash", 2000, 2048};
    EXPECT_FALSE(a == b);
}

TEST(ProtocolCoverage, FileInfo_EqualOperator_DifferentSize) {
    FileInfo a{"file.txt", "hash", 1000, 100};
    FileInfo b{"file.txt", "hash", 1000, 200};
    EXPECT_FALSE(a == b);
}

TEST(ProtocolCoverage, FileInfo_EqualOperator_DefaultSize) {
    // 默认 size = 0
    FileInfo a{"file.txt", "hash", 1000};
    FileInfo b{"file.txt", "hash", 1000, 0};
    EXPECT_TRUE(a == b);
}

// ============================================================================
// FileInfo JSON - size 字段兼容性
// ============================================================================

TEST(ProtocolCoverage, FileInfo_FromJson_WithSize) {
    nlohmann::json j = {
        {"path", "test.txt"},
        {"hash", "abcdef"},
        {"mtime", 12345},
        {"size", 67890}
    };
    
    FileInfo info = j.get<FileInfo>();
    EXPECT_EQ(info.path, "test.txt");
    EXPECT_EQ(info.hash, "abcdef");
    EXPECT_EQ(info.modified_time, 12345);
    EXPECT_EQ(info.size, 67890);
}

TEST(ProtocolCoverage, FileInfo_FromJson_WithoutSize_DefaultsToZero) {
    // 模拟旧版本没有 size 字段
    nlohmann::json j = {
        {"path", "old.txt"},
        {"hash", "oldhash"},
        {"mtime", 100}
    };
    
    FileInfo info = j.get<FileInfo>();
    EXPECT_EQ(info.path, "old.txt");
    EXPECT_EQ(info.hash, "oldhash");
    EXPECT_EQ(info.modified_time, 100);
    EXPECT_EQ(info.size, 0) << "缺少 size 字段时应默认为 0";
}

TEST(ProtocolCoverage, FileInfo_ToJson_IncludesSize) {
    FileInfo info{"file.txt", "hash", 1000, 4096};
    nlohmann::json j = info;
    
    EXPECT_TRUE(j.contains("size"));
    EXPECT_EQ(j["size"].get<uint64_t>(), 4096);
}

TEST(ProtocolCoverage, FileInfo_JsonRoundtrip_WithSize) {
    FileInfo original{"deep/path/file.txt", "sha256hash", 999999, 1048576};
    nlohmann::json j = original;
    FileInfo restored = j.get<FileInfo>();
    
    EXPECT_TRUE(original == restored);
}

TEST(ProtocolCoverage, FileInfo_JsonRoundtrip_ZeroSize) {
    FileInfo original{"file.txt", "hash", 100, 0};
    nlohmann::json j = original;
    FileInfo restored = j.get<FileInfo>();
    
    EXPECT_TRUE(original == restored);
}

TEST(ProtocolCoverage, FileInfo_FromJson_LargeSize) {
    // 测试大文件 size (> 4GB)
    nlohmann::json j = {
        {"path", "big.bin"},
        {"hash", "bighash"},
        {"mtime", 100},
        {"size", 5368709120ULL}  // 5GB
    };
    
    FileInfo info = j.get<FileInfo>();
    EXPECT_EQ(info.size, 5368709120ULL);
}

// ============================================================================
// FileInfo JSON - 异常处理
// ============================================================================

TEST(ProtocolCoverage, FileInfo_FromJson_MissingPath_Throws) {
    nlohmann::json j = {{"hash", "abc"}, {"mtime", 100}};
    EXPECT_THROW(j.get<FileInfo>(), nlohmann::json::out_of_range);
}

TEST(ProtocolCoverage, FileInfo_FromJson_MissingHash_Throws) {
    nlohmann::json j = {{"path", "f.txt"}, {"mtime", 100}};
    EXPECT_THROW(j.get<FileInfo>(), nlohmann::json::out_of_range);
}

TEST(ProtocolCoverage, FileInfo_FromJson_MissingMtime_Throws) {
    nlohmann::json j = {{"path", "f.txt"}, {"hash", "abc"}};
    EXPECT_THROW(j.get<FileInfo>(), nlohmann::json::out_of_range);
}

// ============================================================================
// Protocol 常量 — 非空验证
// ============================================================================

TEST(ProtocolCoverage, AllProtocolConstants_NonEmpty) {
    EXPECT_NE(std::string(Protocol::MSG_TYPE), "");
    EXPECT_NE(std::string(Protocol::MSG_PAYLOAD), "");
    EXPECT_NE(std::string(Protocol::TYPE_SHARE_STATE), "");
    EXPECT_NE(std::string(Protocol::TYPE_REQUEST_FILE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_CHUNK), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_UPDATE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_DELETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_CREATE), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_DELETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_UPDATE_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_DELETE_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_BEGIN), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_ACK), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_COMPLETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_GOODBYE), "");
}

TEST(ProtocolCoverage, ProtocolConstants_Uniqueness) {
    // 所有消息类型常量应该互不相同
    std::set<std::string> types;
    types.insert(Protocol::TYPE_SHARE_STATE);
    types.insert(Protocol::TYPE_REQUEST_FILE);
    types.insert(Protocol::TYPE_FILE_CHUNK);
    types.insert(Protocol::TYPE_FILE_UPDATE);
    types.insert(Protocol::TYPE_FILE_DELETE);
    types.insert(Protocol::TYPE_DIR_CREATE);
    types.insert(Protocol::TYPE_DIR_DELETE);
    types.insert(Protocol::TYPE_FILE_UPDATE_BATCH);
    types.insert(Protocol::TYPE_FILE_DELETE_BATCH);
    types.insert(Protocol::TYPE_DIR_BATCH);
    types.insert(Protocol::TYPE_SYNC_BEGIN);
    types.insert(Protocol::TYPE_SYNC_ACK);
    types.insert(Protocol::TYPE_SYNC_COMPLETE);
    types.insert(Protocol::TYPE_GOODBYE);
    
    EXPECT_EQ(types.size(), 14) << "所有 14 个消息类型常量应该各不相同";
}
