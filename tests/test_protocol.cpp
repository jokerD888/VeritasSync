#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "VeritasSync/Config.h"
#include "VeritasSync/Protocol.h"

using namespace VeritasSync;
using json = nlohmann::json;

// 1. 测试 FileInfo 序列化
TEST(ProtocolTest, FileInfoJsonConversion) {
    FileInfo original{"src/main.cpp", "hash_123456", 1620000000};

    // 序列化
    json j = original;

    // 反序列化
    FileInfo restored = j.get<FileInfo>();

    EXPECT_EQ(restored.path, original.path);
    EXPECT_EQ(restored.hash, original.hash);
    EXPECT_EQ(restored.modified_time, original.modified_time);
}

// 2. 测试 Config 序列化 (防止配置文件解析挂掉)
TEST(ProtocolTest, ConfigJsonConversion) {
    Config cfg;
    cfg.tracker_host = "192.168.1.100";
    cfg.tracker_port = 6666;
    cfg.tasks.push_back({"key1", "source", "/tmp/sync"});

    json j = cfg;
    Config restored = j.get<Config>();

    EXPECT_EQ(restored.tracker_host, "192.168.1.100");
    EXPECT_EQ(restored.tracker_port, 6666);
    ASSERT_EQ(restored.tasks.size(), 1);
    EXPECT_EQ(restored.tasks[0].sync_key, "key1");
}