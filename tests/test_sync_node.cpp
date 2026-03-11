#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/sync/SyncNode.h"

using namespace VeritasSync;

// 全局测试环境
class SyncNodeTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override { init_logger(); }
};
static ::testing::Environment* const node_env =
    ::testing::AddGlobalTestEnvironment(new SyncNodeTestEnvironment());

// ============================================================================
// SyncNode 工厂方法和构造
// ============================================================================

class SyncNodeCreationTest : public ::testing::Test {
protected:
    Config make_config() {
        Config cfg;
        cfg.device_id = "test-device-001";
        cfg.tracker_host = "127.0.0.1";
        cfg.tracker_port = 9988;
        cfg.stun_host = "stun.l.google.com";
        cfg.stun_port = 19302;
        return cfg;
    }
};

TEST_F(SyncNodeCreationTest, Create_ReturnsSharedPtr) {
    SyncTask task;
    task.sync_key = "test-key";
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_create";
    task.mode = SyncMode::OneWay;

    auto node = SyncNode::create(task, make_config());
    ASSERT_NE(node, nullptr);
}

TEST_F(SyncNodeCreationTest, Create_PreservesTaskInfo) {
    SyncTask task;
    task.sync_key = "my-unique-key";
    task.role = "destination";
    task.sync_folder = "test_sync_node_dir_preserve";

    auto node = SyncNode::create(task, make_config());
    EXPECT_EQ(node->get_key(), "my-unique-key");
    EXPECT_EQ(node->get_root_path(), "test_sync_node_dir_preserve");
}

TEST_F(SyncNodeCreationTest, Create_InitialState) {
    SyncTask task{"key1", "source", "test_sync_node_dir_initial"};

    auto node = SyncNode::create(task, make_config());

    // 初始状态：未启动
    EXPECT_FALSE(node->is_started());
    // P2P 管理器初始为空
    EXPECT_EQ(node->get_p2p(), nullptr);
    // Tracker 未连接
    EXPECT_FALSE(node->is_tracker_online());
}

// ============================================================================
// SyncNode 配置验证 (start 时)
// ============================================================================

class SyncNodeValidationTest : public ::testing::Test {
protected:
    Config make_config() {
        Config cfg;
        cfg.device_id = "test-device-002";
        cfg.tracker_host = "127.0.0.1";
        cfg.tracker_port = 9988;
        cfg.stun_host = "stun.l.google.com";
        cfg.stun_port = 19302;
        return cfg;
    }
};

TEST_F(SyncNodeValidationTest, Start_EmptySyncKey_Fails) {
    SyncTask task;
    task.sync_key = "";  // 空的 sync_key
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_empty_key";

    auto node = SyncNode::create(task, make_config());
    node->start();

    // 空 sync_key 应导致启动失败
    EXPECT_FALSE(node->is_started());
}

TEST_F(SyncNodeValidationTest, Start_EmptySyncFolder_Fails) {
    SyncTask task;
    task.sync_key = "valid-key";
    task.role = "source";
    task.sync_folder = "";  // 空的 sync_folder

    auto node = SyncNode::create(task, make_config());
    node->start();

    EXPECT_FALSE(node->is_started());
}

TEST_F(SyncNodeValidationTest, Start_InvalidRole_Fails) {
    SyncTask task;
    task.sync_key = "valid-key";
    task.role = "invalid_role";  // 无效角色
    task.sync_folder = "test_sync_node_dir_invalid_role";

    auto node = SyncNode::create(task, make_config());
    node->start();

    EXPECT_FALSE(node->is_started());
}

TEST_F(SyncNodeValidationTest, Start_ValidSource_Role) {
    SyncTask task;
    task.sync_key = "valid-key";
    task.role = "source";  // 有效角色
    task.sync_folder = "test_sync_node_dir_valid_source_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    auto node = SyncNode::create(task, make_config());
    // 注意：完整 start 需要网络连接（TrackerClient），在内网环境可能失败
    // 但配置验证部分应该通过
    node->start();

    // 只要配置验证通过，is_started 应该为 true
    // （即使后续的 Tracker 连接在内网环境失败，started 标志仍为 true）
    EXPECT_TRUE(node->is_started());

    // 清理
    node->stop();
    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

TEST_F(SyncNodeValidationTest, Start_ValidDestination_Role) {
    SyncTask task;
    task.sync_key = "valid-key";
    task.role = "destination";  // 有效角色
    task.sync_folder = "test_sync_node_dir_valid_dest_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    auto node = SyncNode::create(task, make_config());
    node->start();
    EXPECT_TRUE(node->is_started());

    node->stop();
    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

// ============================================================================
// SyncNode 生命周期管理
// ============================================================================

class SyncNodeLifecycleTest : public ::testing::Test {
protected:
    Config make_config() {
        Config cfg;
        cfg.device_id = "test-device-003";
        cfg.tracker_host = "127.0.0.1";
        cfg.tracker_port = 9988;
        cfg.stun_host = "stun.l.google.com";
        cfg.stun_port = 19302;
        return cfg;
    }
};

TEST_F(SyncNodeLifecycleTest, Stop_WithoutStart_Safe) {
    SyncTask task{"key1", "source", "test_sync_node_dir_stop_nostart"};
    auto node = SyncNode::create(task, make_config());

    // 从未启动就 stop，不应崩溃
    node->stop();
    EXPECT_FALSE(node->is_started());
}

TEST_F(SyncNodeLifecycleTest, DoubleStop_Safe) {
    SyncTask task;
    task.sync_key = "key1";
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_double_stop_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    auto node = SyncNode::create(task, make_config());
    node->start();
    EXPECT_TRUE(node->is_started());

    node->stop();
    EXPECT_FALSE(node->is_started());

    // 第二次 stop 不应崩溃
    node->stop();
    EXPECT_FALSE(node->is_started());

    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

TEST_F(SyncNodeLifecycleTest, DoubleStart_Ignored) {
    SyncTask task;
    task.sync_key = "key1";
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_double_start_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    auto node = SyncNode::create(task, make_config());
    node->start();
    EXPECT_TRUE(node->is_started());

    // 第二次 start 应被忽略
    node->start();
    EXPECT_TRUE(node->is_started());

    node->stop();
    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

TEST_F(SyncNodeLifecycleTest, Destructor_CallsStop) {
    SyncTask task;
    task.sync_key = "key1";
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_dtor_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    {
        auto node = SyncNode::create(task, make_config());
        node->start();
        EXPECT_TRUE(node->is_started());
        // 让 node 超出作用域，析构函数应调用 stop()
    }
    // 不崩溃即通过

    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

TEST_F(SyncNodeLifecycleTest, StopThenRestart) {
    SyncTask task;
    task.sync_key = "key1";
    task.role = "source";
    task.sync_folder = "test_sync_node_dir_restart_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    auto node = SyncNode::create(task, make_config());
    node->start();
    EXPECT_TRUE(node->is_started());

    node->stop();
    EXPECT_FALSE(node->is_started());

    // 重新启动应该可以工作
    node->start();
    EXPECT_TRUE(node->is_started());

    node->stop();
    std::error_code ec;
    std::filesystem::remove_all(task.sync_folder, ec);
}

// ============================================================================
// SyncNode 目录创建
// ============================================================================

class SyncNodeDirectoryTest : public ::testing::Test {
protected:
    Config make_config() {
        Config cfg;
        cfg.device_id = "test-device-004";
        cfg.tracker_host = "127.0.0.1";
        cfg.tracker_port = 9988;
        cfg.stun_host = "stun.l.google.com";
        cfg.stun_port = 19302;
        return cfg;
    }
};

TEST_F(SyncNodeDirectoryTest, Start_CreatesDirectory_IfNotExists) {
    std::string test_dir = "test_sync_node_autodir_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    // 确保目录不存在
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);

    SyncTask task;
    task.sync_key = "key1";
    task.role = "source";
    task.sync_folder = test_dir;

    auto node = SyncNode::create(task, make_config());
    node->start();

    // start 应自动创建目录
    EXPECT_TRUE(std::filesystem::exists(test_dir));

    node->stop();
    std::filesystem::remove_all(test_dir, ec);
}

// ============================================================================
// SyncNode 工厂方法设计验证
// ============================================================================

TEST(SyncNodeFactoryDesign, SharedPtr_CanBeWeak) {
    SyncTask task{"key1", "source", "test_sync_node_dir_weak"};
    Config cfg;
    cfg.device_id = "test";
    cfg.tracker_host = "127.0.0.1";
    cfg.tracker_port = 9988;

    auto node = SyncNode::create(task, cfg);
    std::weak_ptr<SyncNode> weak = node;

    EXPECT_FALSE(weak.expired());

    node.reset();
    EXPECT_TRUE(weak.expired());
}

TEST(SyncNodeFactoryDesign, GetKey_MatchesSyncKey) {
    SyncTask task;
    task.sync_key = "unique-sync-key-xyz";
    task.role = "source";
    task.sync_folder = "/tmp/test";

    Config cfg;
    cfg.device_id = "test";
    cfg.tracker_host = "127.0.0.1";
    cfg.tracker_port = 9988;

    auto node = SyncNode::create(task, cfg);
    EXPECT_EQ(node->get_key(), "unique-sync-key-xyz");
}

TEST(SyncNodeFactoryDesign, GetRootPath_MatchesSyncFolder) {
    SyncTask task;
    task.sync_key = "key1";
    task.role = "destination";
    task.sync_folder = "D:\\Data\\Sync\\folder1";

    Config cfg;
    cfg.device_id = "test";
    cfg.tracker_host = "127.0.0.1";
    cfg.tracker_port = 9988;

    auto node = SyncNode::create(task, cfg);
    EXPECT_EQ(node->get_root_path(), "D:\\Data\\Sync\\folder1");
}
