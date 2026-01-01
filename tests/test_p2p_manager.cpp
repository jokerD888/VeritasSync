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
