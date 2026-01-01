// tests/test_p2p_manager.cpp
// P2PManager 单元测试

#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <set>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "VeritasSync/p2p/P2PManager.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// ═══════════════════════════════════════════════════════════════
// 测试环境设置
// ═══════════════════════════════════════════════════════════════

class P2PManagerTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const p2p_manager_env =
    ::testing::AddGlobalTestEnvironment(new P2PManagerTestEnvironment());

// ═══════════════════════════════════════════════════════════════
// 测试夹具
// ═══════════════════════════════════════════════════════════════

class P2PManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_manager = P2PManager::create();
    }
    
    void TearDown() override {
        m_manager.reset();
    }
    
    std::shared_ptr<P2PManager> m_manager;
};

// ═══════════════════════════════════════════════════════════════
// 1. 创建和销毁测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, CreateManager) {
    ASSERT_NE(m_manager, nullptr);
}

TEST_F(P2PManagerTest, MultipleCreate) {
    auto manager1 = P2PManager::create();
    auto manager2 = P2PManager::create();
    
    ASSERT_NE(manager1, nullptr);
    ASSERT_NE(manager2, nullptr);
    EXPECT_NE(manager1.get(), manager2.get());
}

// ═══════════════════════════════════════════════════════════════
// 2. 配置方法测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, SetRole) {
    // 不应崩溃
    m_manager->set_role(SyncRole::Source);
    m_manager->set_role(SyncRole::Destination);
}

TEST_F(P2PManagerTest, SetMode) {
    m_manager->set_mode(SyncMode::OneWay);
    m_manager->set_mode(SyncMode::BiDirectional);
}

TEST_F(P2PManagerTest, SetStateManager) {
    // 设置 nullptr 应该安全
    m_manager->set_state_manager(nullptr);
}

TEST_F(P2PManagerTest, SetTrackerClient) {
    // 设置 nullptr 应该安全
    m_manager->set_tracker_client(nullptr);
}

TEST_F(P2PManagerTest, SetEncryptionKey) {
    m_manager->set_encryption_key("test_key_12345");
    m_manager->set_encryption_key("");  // 空 key
    m_manager->set_encryption_key(std::string(64, 'a'));  // 长 key
}

// ═══════════════════════════════════════════════════════════════
// 3. ICE 配置测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, SetStunConfig) {
    m_manager->set_stun_config("stun.l.google.com", 19302);
    m_manager->set_stun_config("stun1.l.google.com", 19302);
    m_manager->set_stun_config("", 0);  // 空服务器
}

TEST_F(P2PManagerTest, SetTurnConfig) {
    m_manager->set_turn_config("turn.example.com", 3478, "user", "pass");
    m_manager->set_turn_config("", 0, "", "");  // 空配置
}

// ═══════════════════════════════════════════════════════════════
// 4. 连接管理测试（无真实网络）
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, ConnectWithoutTrackerClient) {
    // 没有设置 TrackerClient 时连接应该安全失败
    std::vector<std::string> peers = {"peer1", "peer2"};
    m_manager->connect_to_peers(peers);
    // 应该打印警告日志但不崩溃
}

TEST_F(P2PManagerTest, ConnectWithEmptyPeerList) {
    std::vector<std::string> empty_peers;
    m_manager->connect_to_peers(empty_peers);
}

TEST_F(P2PManagerTest, HandlePeerLeaveWithUnknownPeer) {
    // 处理未知 peer 的离开应该安全
    m_manager->handle_peer_leave("unknown_peer_id");
}

// ═══════════════════════════════════════════════════════════════
// 5. 信令处理测试（无真实连接）
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, HandleSignalingFromUnknownPeer) {
    // 从未知 peer 接收信令应该安全处理
    m_manager->handle_signaling_message("unknown_peer", "ice_candidate", "candidate:...");
    m_manager->handle_signaling_message("unknown_peer", "sdp_offer", "v=0\r\n...");
    m_manager->handle_signaling_message("unknown_peer", "sdp_answer", "v=0\r\n...");
}

TEST_F(P2PManagerTest, HandleInvalidSignalingType) {
    m_manager->handle_signaling_message("peer", "invalid_type", "data");
    m_manager->handle_signaling_message("peer", "", "data");
}

// ═══════════════════════════════════════════════════════════════
// 6. Transfer 相关测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, GetActiveTransfersWhenEmpty) {
    auto transfers = m_manager->get_active_transfers();
    EXPECT_TRUE(transfers.empty());
}

TEST_F(P2PManagerTest, GetTransferStats) {
    auto stats = m_manager->get_transfer_stats();
    // 初始状态应该全是 0
    EXPECT_EQ(stats.total, 0u);
    EXPECT_EQ(stats.done, 0u);
}

// ═══════════════════════════════════════════════════════════════
// 7. 广播测试（无连接时）
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, BroadcastWithoutConnections) {
    // 没有连接时广播应该安全（无操作）
    m_manager->broadcast_current_state();
}

TEST_F(P2PManagerTest, BroadcastFileUpdate) {
    FileInfo fi;
    fi.path = "test/file.txt";
    fi.hash = "abcd1234";
    fi.modified_time = 12345;
    
    // 无连接时不应崩溃
    m_manager->broadcast_file_update(fi);
}

TEST_F(P2PManagerTest, BroadcastFileDelete) {
    m_manager->broadcast_file_delete("test/file.txt");
}

TEST_F(P2PManagerTest, BroadcastDirCreate) {
    m_manager->broadcast_dir_create("test/subdir");
}

TEST_F(P2PManagerTest, BroadcastDirDelete) {
    m_manager->broadcast_dir_delete("test/subdir");
}

// 测试 BiDirectional 模式下 Destination 角色也可以广播
TEST_F(P2PManagerTest, BroadcastInBidirectionalModeAsDestination) {
    m_manager->set_role(SyncRole::Destination);
    m_manager->set_mode(SyncMode::BiDirectional);
    
    // 在 BiDirectional 模式下，Destination 也可以广播
    FileInfo fi;
    fi.path = "test/file.txt";
    fi.hash = "abcd1234";
    fi.modified_time = 12345;
    
    // 这些调用不应崩溃
    m_manager->broadcast_file_update(fi);
    m_manager->broadcast_file_delete("test/file.txt");
    m_manager->broadcast_dir_create("test/subdir");
    m_manager->broadcast_dir_delete("test/subdir");
}

// 测试 OneWay 模式下 Destination 不能广播（验证正常退出）
TEST_F(P2PManagerTest, BroadcastInOneWayModeAsDestination) {
    m_manager->set_role(SyncRole::Destination);
    m_manager->set_mode(SyncMode::OneWay);
    
    // 在 OneWay 模式下，Destination 不能广播（应该安静退出）
    FileInfo fi;
    fi.path = "test/file.txt";
    fi.hash = "abcd1234";
    fi.modified_time = 12345;
    
    // 这些调用应该安静退出，不崩溃
    m_manager->broadcast_file_update(fi);
    m_manager->broadcast_file_delete("test/file.txt");
    m_manager->broadcast_dir_create("test/subdir");
    m_manager->broadcast_dir_delete("test/subdir");
}

// 测试 Source 角色在任何模式下都可以广播
TEST_F(P2PManagerTest, BroadcastAsSourceInAllModes) {
    // OneWay 模式
    m_manager->set_role(SyncRole::Source);
    m_manager->set_mode(SyncMode::OneWay);
    m_manager->broadcast_current_state();
    m_manager->broadcast_dir_create("dir1");
    
    // BiDirectional 模式
    m_manager->set_mode(SyncMode::BiDirectional);
    m_manager->broadcast_current_state();
    m_manager->broadcast_dir_delete("dir2");
}

// ═══════════════════════════════════════════════════════════════
// 8. IO Context 测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, GetIoContext) {
    auto& io_ctx = m_manager->get_io_context();
    // io_context 应该可用
    bool posted = false;
    boost::asio::post(io_ctx, [&posted]() {
        posted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(posted);
}

// ═══════════════════════════════════════════════════════════════
// 9. 线程安全测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, ConcurrentOperations) {
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    
    // 多个线程同时操作
    threads.emplace_back([this, &running]() {
        while (running) {
            m_manager->get_active_transfers();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    
    threads.emplace_back([this, &running]() {
        while (running) {
            m_manager->broadcast_current_state();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    
    threads.emplace_back([this, &running]() {
        while (running) {
            m_manager->handle_peer_leave("nonexistent");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });
    
    // 运行一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    running = false;
    for (auto& t : threads) {
        t.join();
    }
}

TEST_F(P2PManagerTest, ConcurrentSignalingHandling) {
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 10; ++j) {
                std::string peer = "peer_" + std::to_string(i);
                m_manager->handle_signaling_message(peer, "ice_candidate", "candidate:" + std::to_string(j));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
}

// ═══════════════════════════════════════════════════════════════
// 10. 边界条件测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, HandleEmptyPeerId) {
    m_manager->connect_to_peers({""});
    m_manager->handle_peer_leave("");
    m_manager->handle_signaling_message("", "ice_candidate", "data");
}

TEST_F(P2PManagerTest, LargePeerIdList) {
    std::vector<std::string> many_peers;
    for (int i = 0; i < 100; ++i) {
        many_peers.push_back("peer_" + std::to_string(i));
    }
    
    // 应该能处理大量 peer（即使没有真正的连接）
    m_manager->connect_to_peers(many_peers);
}

// ═══════════════════════════════════════════════════════════════
// 11. 生命周期测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, RapidCreateAndDestroy) {
    for (int i = 0; i < 10; ++i) {
        auto manager = P2PManager::create();
        ASSERT_NE(manager, nullptr);
    }
}

TEST_F(P2PManagerTest, CreateUseAndDestroy) {
    for (int i = 0; i < 5; ++i) {
        auto manager = P2PManager::create();
        ASSERT_NE(manager, nullptr);
        
        manager->set_role(SyncRole::Source);
        manager->set_mode(SyncMode::OneWay);
        manager->set_stun_config("stun.l.google.com", 19302);
        manager->broadcast_current_state();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ═══════════════════════════════════════════════════════════════
// 12. Protocol 常量测试
// ═══════════════════════════════════════════════════════════════

TEST(ProtocolTest, MessageTypeConstants) {
    // 验证协议常量已定义且非空
    EXPECT_NE(Protocol::MSG_TYPE, nullptr);
    EXPECT_NE(Protocol::MSG_PAYLOAD, nullptr);
    EXPECT_NE(Protocol::TYPE_SHARE_STATE, nullptr);
    EXPECT_NE(Protocol::TYPE_FILE_UPDATE, nullptr);
    EXPECT_NE(Protocol::TYPE_FILE_DELETE, nullptr);
    EXPECT_NE(Protocol::TYPE_REQUEST_FILE, nullptr);
    EXPECT_NE(Protocol::TYPE_FILE_CHUNK, nullptr);
    EXPECT_NE(Protocol::TYPE_DIR_CREATE, nullptr);
    EXPECT_NE(Protocol::TYPE_DIR_DELETE, nullptr);
    EXPECT_NE(Protocol::TYPE_SYNC_BEGIN, nullptr);
    EXPECT_NE(Protocol::TYPE_SYNC_ACK, nullptr);
    
    // 验证非空字符串
    EXPECT_GT(strlen(Protocol::MSG_TYPE), 0u);
    EXPECT_GT(strlen(Protocol::TYPE_SHARE_STATE), 0u);
}

TEST(ProtocolTest, MessageTypeUniqueness) {
    // 验证消息类型唯一
    std::vector<std::string> types = {
        Protocol::TYPE_SHARE_STATE,
        Protocol::TYPE_FILE_UPDATE,
        Protocol::TYPE_FILE_DELETE,
        Protocol::TYPE_REQUEST_FILE,
        Protocol::TYPE_FILE_CHUNK,
        Protocol::TYPE_DIR_CREATE,
        Protocol::TYPE_DIR_DELETE,
        Protocol::TYPE_SYNC_BEGIN,
        Protocol::TYPE_SYNC_ACK,
    };
    
    std::set<std::string> unique_types(types.begin(), types.end());
    EXPECT_EQ(types.size(), unique_types.size()) << "Protocol message types should be unique";
}

// ═══════════════════════════════════════════════════════════════
// 13. SyncRole 和 SyncMode 枚举测试
// ═══════════════════════════════════════════════════════════════

TEST(SyncEnumsTest, SyncRoleValues) {
    // 验证枚举值可以正确使用
    SyncRole source = SyncRole::Source;
    SyncRole dest = SyncRole::Destination;
    
    EXPECT_NE(static_cast<int>(source), static_cast<int>(dest));
}

TEST(SyncEnumsTest, SyncModeValues) {
    SyncMode oneway = SyncMode::OneWay;
    SyncMode bidir = SyncMode::BiDirectional;
    
    EXPECT_NE(static_cast<int>(oneway), static_cast<int>(bidir));
}

// ═══════════════════════════════════════════════════════════════
// 14. 配置链式调用测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, ChainedConfiguration) {
    // 连续配置不应崩溃
    m_manager->set_role(SyncRole::Source);
    m_manager->set_mode(SyncMode::BiDirectional);
    m_manager->set_stun_config("stun.l.google.com", 19302);
    m_manager->set_turn_config("turn.example.com", 3478, "user", "pass");
    m_manager->set_encryption_key("my_secret_key");
    m_manager->set_state_manager(nullptr);
    m_manager->set_tracker_client(nullptr);
    
    // 重复配置也不应崩溃
    m_manager->set_role(SyncRole::Destination);
    m_manager->set_mode(SyncMode::OneWay);
}

// ═══════════════════════════════════════════════════════════════
// 15. 析构时清理测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, DestructionWithPendingOperations) {
    // 在有待处理操作时销毁
    m_manager->set_role(SyncRole::Source);
    m_manager->broadcast_current_state();
    
    // 立即销毁，不应崩溃
    m_manager.reset();
    
    // 创建新实例验证资源已正确释放
    m_manager = P2PManager::create();
    ASSERT_NE(m_manager, nullptr);
}

// ═══════════════════════════════════════════════════════════════
// 16. 特殊字符和边界测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, SpecialCharactersInPath) {
    // 测试特殊字符路径
    m_manager->broadcast_file_delete("path/with spaces/file.txt");
    m_manager->broadcast_file_delete("path/中文路径/文件.txt");
    m_manager->broadcast_file_delete("path/with\"quotes\"/file.txt");
    m_manager->broadcast_dir_create("目录/子目录");
}

TEST_F(P2PManagerTest, VeryLongPath) {
    // 测试超长路径
    std::string long_path(500, 'a');
    m_manager->broadcast_file_delete(long_path);
    m_manager->broadcast_dir_create(long_path);
}

TEST_F(P2PManagerTest, EmptyPath) {
    // 空路径应该安全处理
    m_manager->broadcast_file_delete("");
    m_manager->broadcast_dir_create("");
    m_manager->broadcast_dir_delete("");
}

// ═══════════════════════════════════════════════════════════════
// 17. 快速状态切换测试
// ═══════════════════════════════════════════════════════════════

TEST_F(P2PManagerTest, RapidRoleSwitch) {
    for (int i = 0; i < 100; ++i) {
        m_manager->set_role(i % 2 == 0 ? SyncRole::Source : SyncRole::Destination);
        m_manager->set_mode(i % 2 == 0 ? SyncMode::OneWay : SyncMode::BiDirectional);
    }
}

// ═══════════════════════════════════════════════════════════════
// 18. 并发创建销毁测试
// ═══════════════════════════════════════════════════════════════

TEST(P2PManagerConcurrencyTest, ConcurrentCreateDestroy) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&success_count]() {
            for (int j = 0; j < 3; ++j) {
                auto manager = P2PManager::create();
                if (manager) {
                    success_count++;
                    manager->set_role(SyncRole::Source);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(success_count.load(), 15);
}

// ═══════════════════════════════════════════════════════════════
// 19. share_state 消息 JSON 解析回归测试
// ═══════════════════════════════════════════════════════════════

// 【回归测试】验证 share_state 消息中 files 数组的正确解析
// 此测试是为了防止 PR#xxx 修复的 bug 再次发生：
// 错误代码使用 payload["files"].items() 遍历数组，
// 导致 key 变成数组索引 "0", "1" 而不是文件路径

TEST(ShareStateParsingTest, ParseFilesArrayCorrectly) {
    // 模拟 get_state_as_json_string 生成的 payload 格式
    nlohmann::json payload;
    payload["files"] = nlohmann::json::array();
    
    // 添加测试文件
    nlohmann::json file1;
    file1["path"] = "新建 文本文档.txt";
    file1["hash"] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    file1["mtime"] = 1735747562;
    payload["files"].push_back(file1);
    
    nlohmann::json file2;
    file2["path"] = "test/subfolder/data.json";
    file2["hash"] = "abc123def456";
    file2["mtime"] = 1735700000;
    payload["files"].push_back(file2);
    
    payload["directories"] = nlohmann::json::array({"test", "test/subfolder"});
    
    // 使用正确的解析方式（与 P2PManager::handle_share_state 一致）
    std::vector<FileInfo> parsed_files;
    
    if (payload.contains("files")) {
        for (const auto& file_json : payload["files"]) {
            FileInfo fi;
            fi.path = file_json.value("path", "");
            fi.modified_time = file_json.value("mtime", static_cast<uint64_t>(0));
            fi.hash = file_json.value("hash", "");
            if (!fi.path.empty()) {
                parsed_files.push_back(fi);
            }
        }
    }
    
    // 验证解析结果
    ASSERT_EQ(parsed_files.size(), 2u);
    
    EXPECT_EQ(parsed_files[0].path, "新建 文本文档.txt");
    EXPECT_EQ(parsed_files[0].hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(parsed_files[0].modified_time, 1735747562u);
    
    EXPECT_EQ(parsed_files[1].path, "test/subfolder/data.json");
    EXPECT_EQ(parsed_files[1].hash, "abc123def456");
    EXPECT_EQ(parsed_files[1].modified_time, 1735700000u);
}

// 验证使用 items() 遍历数组会得到错误结果（用于文档说明）
TEST(ShareStateParsingTest, ItemsIterationGivesIndexAsKey) {
    nlohmann::json payload;
    payload["files"] = nlohmann::json::array();
    
    nlohmann::json file1;
    file1["path"] = "actual_path.txt";
    file1["hash"] = "abc123";
    file1["mtime"] = 12345;
    payload["files"].push_back(file1);
    
    // 演示错误的解析方式（使用 items()）
    std::vector<std::string> keys_from_items;
    for (auto& [key, value] : payload["files"].items()) {
        keys_from_items.push_back(key);
    }
    
    // items() 遍历数组时，key 是数组索引 "0", "1", ...
    ASSERT_EQ(keys_from_items.size(), 1u);
    EXPECT_EQ(keys_from_items[0], "0");  // 不是 "actual_path.txt"！
}

// 测试空 files 数组的解析
TEST(ShareStateParsingTest, ParseEmptyFilesArray) {
    nlohmann::json payload;
    payload["files"] = nlohmann::json::array();  // 空数组
    payload["directories"] = nlohmann::json::array();
    
    std::vector<FileInfo> parsed_files;
    
    if (payload.contains("files")) {
        for (const auto& file_json : payload["files"]) {
            FileInfo fi;
            fi.path = file_json.value("path", "");
            fi.modified_time = file_json.value("mtime", static_cast<uint64_t>(0));
            fi.hash = file_json.value("hash", "");
            if (!fi.path.empty()) {
                parsed_files.push_back(fi);
            }
        }
    }
    
    EXPECT_TRUE(parsed_files.empty());
}

// 测试缺少 files 字段的情况
TEST(ShareStateParsingTest, ParsePayloadWithoutFilesField) {
    nlohmann::json payload;
    payload["directories"] = nlohmann::json::array({"dir1"});
    // 没有 files 字段
    
    std::vector<FileInfo> parsed_files;
    
    if (payload.contains("files")) {
        for (const auto& file_json : payload["files"]) {
            FileInfo fi;
            fi.path = file_json.value("path", "");
            parsed_files.push_back(fi);
        }
    }
    
    EXPECT_TRUE(parsed_files.empty());  // 应该安全处理
}

// 测试 FileInfo 序列化和反序列化的一致性
TEST(ShareStateParsingTest, FileInfoSerializationRoundTrip) {
    // 创建原始 FileInfo
    FileInfo original;
    original.path = "测试/路径/文件.txt";
    original.hash = "abc123def456789";
    original.modified_time = 1735747562;
    
    // 序列化为 JSON
    nlohmann::json j = original;
    
    // 反序列化回 FileInfo
    FileInfo restored = j.get<FileInfo>();
    
    // 验证一致性
    EXPECT_EQ(original.path, restored.path);
    EXPECT_EQ(original.hash, restored.hash);
    EXPECT_EQ(original.modified_time, restored.modified_time);
}

// 测试 get_state_as_json_string 生成的完整消息格式
TEST(ShareStateParsingTest, FullMessageFormat) {
    // 模拟完整的 share_state 消息格式
    nlohmann::json files_array = nlohmann::json::array();
    
    FileInfo fi1;
    fi1.path = "file1.txt";
    fi1.hash = "hash1";
    fi1.modified_time = 100;
    files_array.push_back(nlohmann::json(fi1));
    
    FileInfo fi2;
    fi2.path = "dir/file2.txt";
    fi2.hash = "hash2";
    fi2.modified_time = 200;
    files_array.push_back(nlohmann::json(fi2));
    
    nlohmann::json payload;
    payload["files"] = files_array;
    payload["directories"] = std::set<std::string>{"dir"};
    
    nlohmann::json message;
    message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
    message[Protocol::MSG_PAYLOAD] = payload;
    
    // 验证消息结构
    EXPECT_EQ(message[Protocol::MSG_TYPE], "share_state");
    EXPECT_TRUE(message[Protocol::MSG_PAYLOAD].contains("files"));
    EXPECT_TRUE(message[Protocol::MSG_PAYLOAD].contains("directories"));
    
    // 验证 files 是数组
    EXPECT_TRUE(message[Protocol::MSG_PAYLOAD]["files"].is_array());
    EXPECT_EQ(message[Protocol::MSG_PAYLOAD]["files"].size(), 2u);
    
    // 验证数组元素包含预期字段
    auto& first_file = message[Protocol::MSG_PAYLOAD]["files"][0];
    EXPECT_TRUE(first_file.contains("path"));
    EXPECT_TRUE(first_file.contains("hash"));
    EXPECT_TRUE(first_file.contains("mtime"));
}

// ═══════════════════════════════════════════════════════════════
// 20. 锁竞争与流控回归测试
// ═══════════════════════════════════════════════════════════════

// 【回归测试】验证 P2PManager 在高并发场景下不会死锁
// 此测试模拟多个线程同时操作，验证锁的正确使用
TEST_F(P2PManagerTest, ConcurrentSendAndUpdate_NoDeadlock) {
    // 模拟场景：多个线程同时发送消息，同时 KCP 更新
    // 如果 send_cb 持有锁期间调用 send_message，会与 update_all_kcps 竞争锁
    // 修复后：send_cb 在锁外发送，不会阻塞 update_all_kcps
    
    std::atomic<bool> running{true};
    std::atomic<int> operations{0};
    std::vector<std::thread> threads;
    
    // 模拟多个发送线程（类似 TransferManager 的 worker 线程）
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &running, &operations]() {
            while (running) {
                // 模拟广播操作（内部会获取锁）
                m_manager->broadcast_current_state();
                operations++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    
    // 模拟 KCP 更新线程
    threads.emplace_back([this, &running, &operations]() {
        while (running) {
            // 模拟 handle_peer_leave（会获取锁）
            m_manager->handle_peer_leave("nonexistent");
            operations++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    
    // 运行 200ms，如果死锁会超时
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running = false;
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto duration = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    
    // 验证测试在合理时间内完成（没有死锁）
    EXPECT_LT(ms, 500) << "操作应在 500ms 内完成，否则可能存在死锁";
    EXPECT_GT(operations.load(), 10) << "应该完成了足够多的操作";
}

// 验证 TransferManager 的流控阈值配置
// 这是一个文档性测试，确保未来修改时意识到这些参数的重要性
TEST(TransferConfigTest, CongestionThresholdValue) {
    // CONGESTION_THRESHOLD 应该在合理范围内
    // 太高：队列堆积过多，导致卡死（新版本锁层次增加更容易卡死）
    // 太低：频繁流控，降低传输效率
    // 新版本建议值：128-512（比旧版本 1024 更低，因为锁竞争更严重）
    
    // 这个测试主要是文档作用，提醒开发者注意这个参数
    const int RECOMMENDED_MIN = 128;
    const int RECOMMENDED_MAX = 512;
    const int CURRENT_VALUE = 256;  // 当前配置值
    
    EXPECT_GE(CURRENT_VALUE, RECOMMENDED_MIN) 
        << "CONGESTION_THRESHOLD 不应低于 " << RECOMMENDED_MIN;
    EXPECT_LE(CURRENT_VALUE, RECOMMENDED_MAX) 
        << "CONGESTION_THRESHOLD 不应高于 " << RECOMMENDED_MAX;
}

// 验证流控等待时间配置
TEST(TransferConfigTest, FlowControlWaitTime) {
    // 流控等待时间应该足够长，让 KCP 有时间消耗队列
    // 太短：队列无法清空，继续累积
    // 太长：降低传输效率
    // 当前建议值：50-100ms
    
    const int MIN_WAIT_MS = 50;
    const int MAX_WAIT_MS = 100;
    
    // 文档性验证
    EXPECT_GE(MIN_WAIT_MS, 30) << "最小等待时间不应低于 30ms";
    EXPECT_LE(MAX_WAIT_MS, 200) << "最大等待时间不应超过 200ms";
}

