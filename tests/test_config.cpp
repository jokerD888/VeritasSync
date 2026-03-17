#include "test_helpers.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Config.h"

using namespace VeritasSync;
using json = nlohmann::json;

REGISTER_VERITAS_TEST_ENV();

// ============================================================================
// SyncTask 序列化/反序列化
// ============================================================================

class SyncTaskTest : public ::testing::Test {};

TEST_F(SyncTaskTest, JsonRoundTrip_Basic) {
    SyncTask task;
    task.sync_key = "test-key-123";
    task.role = "source";
    task.sync_folder = "/home/user/sync";
    task.mode = SyncMode::OneWay;

    json j = task;
    SyncTask restored = j.get<SyncTask>();

    EXPECT_EQ(restored.sync_key, "test-key-123");
    EXPECT_EQ(restored.role, "source");
    EXPECT_EQ(restored.sync_folder, "/home/user/sync");
    EXPECT_EQ(restored.mode, SyncMode::OneWay);
}

TEST_F(SyncTaskTest, JsonRoundTrip_BiDirectional) {
    SyncTask task;
    task.sync_key = "bidirectional-key";
    task.role = "destination";
    task.sync_folder = "D:\\Sync\\folder";
    task.mode = SyncMode::BiDirectional;

    json j = task;
    SyncTask restored = j.get<SyncTask>();

    EXPECT_EQ(restored.mode, SyncMode::BiDirectional);
    EXPECT_EQ(restored.role, "destination");
}

TEST_F(SyncTaskTest, FromJson_MissingMode_DefaultsToOneWay) {
    json j = {{"sync_key", "key1"}, {"role", "source"}, {"sync_folder", "/tmp"}};
    // 不包含 "mode" 字段
    SyncTask task = j.get<SyncTask>();
    EXPECT_EQ(task.mode, SyncMode::OneWay);
}

TEST_F(SyncTaskTest, FromJson_MissingRequiredField_Throws) {
    json j = {{"sync_key", "key1"}, {"role", "source"}};
    // 缺少 sync_folder
    EXPECT_THROW(j.get<SyncTask>(), nlohmann::json::out_of_range);
}

// ============================================================================
// Config 序列化/反序列化
// ============================================================================

class ConfigTest : public ::testing::Test {};

TEST_F(ConfigTest, DefaultValues) {
    Config cfg;
    EXPECT_EQ(cfg.tracker_port, 9988);
    EXPECT_EQ(cfg.stun_host, "stun.l.google.com");
    EXPECT_EQ(cfg.stun_port, 19302);
    EXPECT_EQ(cfg.turn_port, 3478);
    EXPECT_EQ(cfg.log_level, "info");
    EXPECT_EQ(cfg.kcp_update_interval_ms, 20u);
    EXPECT_EQ(cfg.chunk_size, 16384u);
    EXPECT_EQ(cfg.kcp_window_size, 256u);
    EXPECT_EQ(cfg.webui_port, 8800);
    EXPECT_TRUE(cfg.turn_host.empty());
    EXPECT_TRUE(cfg.tasks.empty());
}

TEST_F(ConfigTest, JsonRoundTrip_Full) {
    Config cfg;
    cfg.device_id = "test-device-uuid";
    cfg.tracker_host = "192.168.1.100";
    cfg.tracker_port = 7777;
    cfg.stun_host = "stun.example.com";
    cfg.stun_port = 3478;
    cfg.turn_host = "turn.example.com";
    cfg.turn_port = 5349;
    cfg.turn_username = "user1";
    cfg.turn_password = "pass1";
    cfg.log_level = "debug";
    cfg.libjuice_log_level = "warn";
    cfg.kcp_update_interval_ms = 50;
    cfg.chunk_size = 32768;
    cfg.kcp_window_size = 512;
    cfg.file_hash_retry_delay_ms = 500;
    cfg.webui_port = 9900;
    cfg.tasks.push_back({"key1", "source", "/tmp/src", SyncMode::OneWay});
    cfg.tasks.push_back({"key2", "destination", "/tmp/dst", SyncMode::BiDirectional});

    json j = cfg;
    Config restored = j.get<Config>();

    EXPECT_EQ(restored.device_id, "test-device-uuid");
    EXPECT_EQ(restored.tracker_host, "192.168.1.100");
    EXPECT_EQ(restored.tracker_port, 7777);
    EXPECT_EQ(restored.stun_host, "stun.example.com");
    EXPECT_EQ(restored.stun_port, 3478);
    EXPECT_EQ(restored.turn_host, "turn.example.com");
    EXPECT_EQ(restored.turn_port, 5349);
    EXPECT_EQ(restored.turn_username, "user1");
    EXPECT_EQ(restored.turn_password, "pass1");
    EXPECT_EQ(restored.log_level, "debug");
    EXPECT_EQ(restored.libjuice_log_level, "warn");
    EXPECT_EQ(restored.kcp_update_interval_ms, 50u);
    EXPECT_EQ(restored.chunk_size, 32768u);
    EXPECT_EQ(restored.kcp_window_size, 512u);
    EXPECT_EQ(restored.file_hash_retry_delay_ms, 500u);
    EXPECT_EQ(restored.webui_port, 9900);
    ASSERT_EQ(restored.tasks.size(), 2);
    EXPECT_EQ(restored.tasks[0].sync_key, "key1");
    EXPECT_EQ(restored.tasks[1].mode, SyncMode::BiDirectional);
}

TEST_F(ConfigTest, FromJson_OptionalFields_UseDefaults) {
    // 最小配置：只有必填字段
    json j = {
        {"tracker_host", "10.0.0.1"},
        {"tracker_port", 9988},
        {"tasks", json::array()}
    };

    Config cfg = j.get<Config>();
    EXPECT_EQ(cfg.tracker_host, "10.0.0.1");
    // 可选字段应使用默认值
    EXPECT_EQ(cfg.stun_host, "stun.l.google.com");
    EXPECT_EQ(cfg.log_level, "info");
    EXPECT_EQ(cfg.kcp_update_interval_ms, 20u);
    EXPECT_EQ(cfg.webui_port, 8800);
    EXPECT_TRUE(cfg.device_id.empty());
}

TEST_F(ConfigTest, FromJson_WithWebuiPort) {
    json j = {
        {"tracker_host", "10.0.0.1"},
        {"tracker_port", 9988},
        {"webui_port", 3000},
        {"tasks", json::array()}
    };

    Config cfg = j.get<Config>();
    EXPECT_EQ(cfg.webui_port, 3000);
}

TEST_F(ConfigTest, SyncMode_SerializationValues) {
    json j_oneway = SyncMode::OneWay;
    EXPECT_EQ(j_oneway.get<std::string>(), "oneway");

    json j_bidi = SyncMode::BiDirectional;
    EXPECT_EQ(j_bidi.get<std::string>(), "bidirectional");
}

// ============================================================================
// validate_config 测试
// ============================================================================

class ConfigValidationTest : public ::testing::Test {
protected:
    // 构造一个合法的默认 Config
    Config make_valid_config() {
        Config cfg;
        cfg.device_id = "some-uuid-1234";
        cfg.tracker_host = "192.168.1.1";
        cfg.tracker_port = 9988;
        cfg.stun_host = "stun.l.google.com";
        cfg.stun_port = 19302;
        cfg.webui_port = 8800;
        cfg.log_level = "info";
        cfg.kcp_update_interval_ms = 20;
        cfg.chunk_size = 16384;
        cfg.kcp_window_size = 256;
        cfg.tasks.push_back({"key1", "source", "/tmp/sync"});
        return cfg;
    }
};

TEST_F(ConfigValidationTest, ValidConfig_NoErrors) {
    auto cfg = make_valid_config();
    auto errors = validate_config(cfg);
    EXPECT_TRUE(errors.empty()) << "Valid config should have no errors, got: " << errors.size();
}

TEST_F(ConfigValidationTest, EmptyTrackerHost) {
    auto cfg = make_valid_config();
    cfg.tracker_host = "";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    // 检查是否包含 tracker_host 相关错误
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("tracker_host") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Should report tracker_host error";
}

TEST_F(ConfigValidationTest, EmptyStunHost) {
    auto cfg = make_valid_config();
    cfg.stun_host = "";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("stun_host") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Should report stun_host error";
}

TEST_F(ConfigValidationTest, EmptyDeviceId) {
    auto cfg = make_valid_config();
    cfg.device_id = "";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("device_id") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Should report device_id error";
}

TEST_F(ConfigValidationTest, ZeroPort_TrackerPort) {
    auto cfg = make_valid_config();
    cfg.tracker_port = 0;
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("tracker_port") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidationTest, ZeroPort_StunPort) {
    auto cfg = make_valid_config();
    cfg.stun_port = 0;
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, ZeroPort_WebuiPort) {
    auto cfg = make_valid_config();
    cfg.webui_port = 0;
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, TurnPort_OnlyCheckedWhenTurnHostSet) {
    auto cfg = make_valid_config();

    // turn_host 为空时，turn_port = 0 不应报错
    cfg.turn_host = "";
    cfg.turn_port = 0;
    auto errors = validate_config(cfg);
    // 应该没有 turn_port 相关错误
    bool found_turn = false;
    for (const auto& e : errors) {
        if (e.find("turn_port") != std::string::npos) { found_turn = true; break; }
    }
    EXPECT_FALSE(found_turn) << "turn_port should not be checked when turn_host is empty";

    // turn_host 非空时，turn_port = 0 应报错
    cfg.turn_host = "turn.example.com";
    cfg.turn_port = 0;
    errors = validate_config(cfg);
    found_turn = false;
    for (const auto& e : errors) {
        if (e.find("turn_port") != std::string::npos) { found_turn = true; break; }
    }
    EXPECT_TRUE(found_turn) << "turn_port should be checked when turn_host is set";
}

TEST_F(ConfigValidationTest, InvalidLogLevel) {
    auto cfg = make_valid_config();
    cfg.log_level = "verbose";  // 无效
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("log_level") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidationTest, ValidLogLevels) {
    auto cfg = make_valid_config();
    std::vector<std::string> valid = {"debug", "info", "warn", "warning", "error", "err", "critical", "off"};
    for (const auto& level : valid) {
        cfg.log_level = level;
        auto errors = validate_config(cfg);
        bool found = false;
        for (const auto& e : errors) {
            if (e.find("log_level") != std::string::npos) { found = true; break; }
        }
        EXPECT_FALSE(found) << "Log level '" << level << "' should be valid";
    }
}

TEST_F(ConfigValidationTest, KcpUpdateInterval_TooLow) {
    auto cfg = make_valid_config();
    cfg.kcp_update_interval_ms = 3;  // 低于 5
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, KcpUpdateInterval_TooHigh) {
    auto cfg = make_valid_config();
    cfg.kcp_update_interval_ms = 600;  // 高于 500
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, KcpUpdateInterval_BoundaryValid) {
    auto cfg = make_valid_config();
    cfg.kcp_update_interval_ms = 5;
    EXPECT_TRUE(validate_config(cfg).empty());

    cfg.kcp_update_interval_ms = 500;
    EXPECT_TRUE(validate_config(cfg).empty());
}

TEST_F(ConfigValidationTest, ChunkSize_TooSmall) {
    auto cfg = make_valid_config();
    cfg.chunk_size = 512;  // 低于 1024
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, ChunkSize_TooLarge) {
    auto cfg = make_valid_config();
    cfg.chunk_size = 2 * 1024 * 1024;  // 2MB，超过 1MB
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());
}

TEST_F(ConfigValidationTest, KcpWindowSize_OutOfRange) {
    auto cfg = make_valid_config();
    cfg.kcp_window_size = 10;  // 低于 16
    EXPECT_FALSE(validate_config(cfg).empty());

    cfg.kcp_window_size = 5000;  // 高于 4096
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST_F(ConfigValidationTest, Task_EmptySyncKey) {
    auto cfg = make_valid_config();
    cfg.tasks[0].sync_key = "";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("sync_key") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidationTest, Task_InvalidSyncKeyCharacters) {
    auto cfg = make_valid_config();
    cfg.tasks[0].sync_key = "../evil/key";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("sync_key") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(ConfigSyncKeyValidationTest, ValidAndInvalidSyncKey) {
    EXPECT_TRUE(is_valid_sync_key("sync-ABC_123"));
    EXPECT_FALSE(is_valid_sync_key(""));
    EXPECT_FALSE(is_valid_sync_key("../escape"));
    EXPECT_FALSE(is_valid_sync_key("bad/key"));
    EXPECT_FALSE(is_valid_sync_key("bad\\key"));
    EXPECT_FALSE(is_valid_sync_key("bad.key"));
}

TEST_F(ConfigValidationTest, Task_InvalidRole) {
    auto cfg = make_valid_config();
    cfg.tasks[0].role = "observer";  // 无效
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("role") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidationTest, Task_EmptySyncFolder) {
    auto cfg = make_valid_config();
    cfg.tasks[0].sync_folder = "";
    auto errors = validate_config(cfg);
    EXPECT_FALSE(errors.empty());

    bool found = false;
    for (const auto& e : errors) {
        if (e.find("sync_folder") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigValidationTest, MultipleTasks_MultipleErrors) {
    auto cfg = make_valid_config();
    cfg.tasks.push_back({"", "invalid_role", ""});
    auto errors = validate_config(cfg);
    // 第二个 task 应有 3 个错误：sync_key 空, role 无效, sync_folder 空
    int task1_errors = 0;
    for (const auto& e : errors) {
        if (e.find("tasks[1]") != std::string::npos) { task1_errors++; }
    }
    EXPECT_GE(task1_errors, 3);
}

TEST_F(ConfigValidationTest, ValidConfig_SourceAndDestination) {
    auto cfg = make_valid_config();
    cfg.tasks.clear();
    cfg.tasks.push_back({"key-a", "source", "/sync/a"});
    cfg.tasks.push_back({"key-b", "destination", "/sync/b"});
    auto errors = validate_config(cfg);
    EXPECT_TRUE(errors.empty());
}

// ============================================================================
// UUID 生成
// ============================================================================

TEST(UuidTest, GenerateUuidV4_NotEmpty) {
    std::string uuid = generate_uuid_v4();
    EXPECT_FALSE(uuid.empty());
}

TEST(UuidTest, GenerateUuidV4_UniqueEachCall) {
    std::string uuid1 = generate_uuid_v4();
    std::string uuid2 = generate_uuid_v4();
    EXPECT_NE(uuid1, uuid2);
}

TEST(UuidTest, GenerateUuidV4_CorrectFormat) {
    std::string uuid = generate_uuid_v4();
    // UUID v4 格式: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 字符)
    EXPECT_EQ(uuid.size(), 36u);
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');
}

// ============================================================================
// load_config_or_create_default
// ============================================================================

class ConfigFileTest : public ::testing::Test {
protected:
    std::string temp_config_path;

    void SetUp() override {
        temp_config_path = "test_config_temp_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(temp_config_path, ec);
    }
};

TEST_F(ConfigFileTest, CreateDefault_WhenFileNotExist) {
    // 确保文件不存在
    std::error_code ec;
    std::filesystem::remove(temp_config_path, ec);

    Config cfg = load_config_or_create_default(temp_config_path);

    // 应该自动生成 device_id
    EXPECT_FALSE(cfg.device_id.empty());
    // 文件应该被创建
    EXPECT_TRUE(std::filesystem::exists(temp_config_path));
}

TEST_F(ConfigFileTest, LoadExisting_PreservesValues) {
    // 先创建一个配置文件
    Config original;
    original.device_id = "existing-device-id";
    original.tracker_host = "10.0.0.5";
    original.tracker_port = 1234;
    original.webui_port = 3000;
    original.tasks.push_back({"my-key", "source", "/data/sync"});

    {
        std::ofstream f(temp_config_path);
        f << json(original).dump(4);
    }

    Config loaded = load_config_or_create_default(temp_config_path);

    EXPECT_EQ(loaded.device_id, "existing-device-id");
    EXPECT_EQ(loaded.tracker_host, "10.0.0.5");
    EXPECT_EQ(loaded.tracker_port, 1234);
    EXPECT_EQ(loaded.webui_port, 3000);
    ASSERT_EQ(loaded.tasks.size(), 1);
    EXPECT_EQ(loaded.tasks[0].sync_key, "my-key");
}

TEST_F(ConfigFileTest, LoadExisting_GeneratesDeviceId_IfMissing) {
    // 创建一个没有 device_id 的配置文件
    json j = {
        {"tracker_host", "10.0.0.1"},
        {"tracker_port", 9988},
        {"tasks", json::array()}
    };
    {
        std::ofstream f(temp_config_path);
        f << j.dump(4);
    }

    Config loaded = load_config_or_create_default(temp_config_path);
    EXPECT_FALSE(loaded.device_id.empty()) << "Should auto-generate device_id";
}
