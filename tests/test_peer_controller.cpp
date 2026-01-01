// tests/test_peer_controller.cpp
// PeerController 单元测试

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// ═══════════════════════════════════════════════════════════════
// 测试环境设置
// ═══════════════════════════════════════════════════════════════

class PeerControllerTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const peer_controller_env =
    ::testing::AddGlobalTestEnvironment(new PeerControllerTestEnvironment());

// ═══════════════════════════════════════════════════════════════
// 测试夹具
// ═══════════════════════════════════════════════════════════════

class PeerControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建 io_context 和工作线程
        m_work_guard = std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(m_io_context.get_executor());
        
        m_io_thread = std::thread([this]() {
            m_io_context.run();
        });
    }
    
    void TearDown() override {
        m_work_guard.reset();
        m_io_context.stop();
        if (m_io_thread.joinable()) {
            m_io_thread.join();
        }
    }
    
    uint32_t get_current_ms() {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
    
    // 创建默认的 ICE 配置（使用公共 STUN 服务器）
    IceConfig create_ice_config() {
        IceConfig config;
        config.stun_host = "stun.l.google.com";
        config.stun_port = 19302;
        return config;
    }
    
    boost::asio::io_context m_io_context;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> m_work_guard;
    std::thread m_io_thread;
};

// ═══════════════════════════════════════════════════════════════
// 1. 创建和销毁测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, CreateController) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    callbacks.on_message_received = [](const std::string&) {};
    
    auto controller = PeerController::create(
        "self_id_123",
        "peer_id_456",
        m_io_context,
        create_ice_config(),
        std::move(callbacks)
    );
    
    ASSERT_NE(controller, nullptr);
    EXPECT_EQ(controller->get_self_id(), "self_id_123");
    EXPECT_EQ(controller->get_peer_id(), "peer_id_456");
    EXPECT_TRUE(controller->is_valid());
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
}

TEST_F(PeerControllerTest, OfferSideDetermination) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // self_id < peer_id -> offer side
    auto controller1 = PeerController::create(
        "aaa", "zzz", m_io_context, create_ice_config(), callbacks);
    ASSERT_NE(controller1, nullptr);
    EXPECT_TRUE(controller1->is_offer_side());
    
    // self_id > peer_id -> answer side
    auto controller2 = PeerController::create(
        "zzz", "aaa", m_io_context, create_ice_config(), callbacks);
    ASSERT_NE(controller2, nullptr);
    EXPECT_FALSE(controller2->is_offer_side());
}

TEST_F(PeerControllerTest, CloseController) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    EXPECT_TRUE(controller->is_valid());
    
    controller->close();
    
    EXPECT_FALSE(controller->is_valid());
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
}

// ═══════════════════════════════════════════════════════════════
// 2. Conv ID 计算测试（关键！）
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, ConvIdConsistencyBetweenPeers) {
    // 这是最重要的测试：验证两端的 Conv ID 是否一致
    
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // 模拟 A 连接 B
    auto controller_a = PeerController::create(
        "peer_A", "peer_B", m_io_context, create_ice_config(), callbacks);
    
    // 模拟 B 连接 A
    auto controller_b = PeerController::create(
        "peer_B", "peer_A", m_io_context, create_ice_config(), callbacks);
    
    ASSERT_NE(controller_a, nullptr);
    ASSERT_NE(controller_b, nullptr);
    
    // 验证角色判断正确
    EXPECT_TRUE(controller_a->is_offer_side());   // A < B
    EXPECT_FALSE(controller_b->is_offer_side());  // B > A
    
    // 注意：我们无法直接访问 calculate_conv()，因为它是 private 的
    // 但我们可以通过观察 KCP 创建时的 conv 来验证（需要日志或其他方式）
    // 这里我们只验证基本的创建成功
}

// ═══════════════════════════════════════════════════════════════
// 3. 状态查询测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, InitialState) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 初始状态
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
    EXPECT_FALSE(controller->is_connected());
    EXPECT_TRUE(controller->is_valid());
    EXPECT_EQ(controller->get_connection_type(), IceConnectionType::None);
    EXPECT_EQ(controller->get_kcp_wait_send(), 0);
}

TEST_F(PeerControllerTest, SyncSessionStateInitialization) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 同步会话状态初始化
    EXPECT_EQ(controller->sync_session_id.load(), 0);
    EXPECT_EQ(controller->expected_file_count.load(), 0);
    EXPECT_EQ(controller->expected_dir_count.load(), 0);
    EXPECT_EQ(controller->received_file_count.load(), 0);
    EXPECT_EQ(controller->received_dir_count.load(), 0);
    EXPECT_EQ(controller->connected_at_ts.load(), 0);
}

// ═══════════════════════════════════════════════════════════════
// 4. 信令类型测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, SignalingTypeForOfferSide) {
    std::vector<std::pair<std::string, std::string>> signals;
    
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [&signals](const std::string& type, const std::string& payload) {
        signals.emplace_back(type, payload);
    };
    
    // self_id < peer_id -> offer side
    auto controller = PeerController::create(
        "aaa", "zzz", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    EXPECT_TRUE(controller->is_offer_side());
    
    // 发起连接
    controller->initiate_connection();
    
    // 等待 ICE 收集（这在实际测试中可能需要网络）
    // 由于我们使用真实的 STUN 服务器，这个测试可能需要网络连接
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 验证状态变为 Connecting
    EXPECT_EQ(controller->get_state(), PeerState::Connecting);
}

TEST_F(PeerControllerTest, HandleSignalingMessage) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 测试处理不同类型的信令
    // 注意：这些操作需要有效的 ICE agent
    controller->handle_signaling("ice_candidate", "candidate:...");
    controller->handle_signaling("sdp_offer", "v=0\r\n...");
    controller->handle_signaling("sdp_answer", "v=0\r\n...");
    controller->handle_signaling("ice_gathering_done", "");
    
    // 最基本的验证：没有崩溃
    EXPECT_TRUE(controller->is_valid());
}

// ═══════════════════════════════════════════════════════════════
// 5. 消息发送测试（需要连接建立后）
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, SendMessageWithoutConnection) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 未连接时发送消息应该返回 -1
    int result = controller->send_message(std::string("Hello"));
    EXPECT_EQ(result, -1);  // KCP 未初始化
}

// ═══════════════════════════════════════════════════════════════
// 6. 资源管理测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, GetIceTransportAndKcpSession) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // IceTransport 应该存在
    auto ice = controller->get_ice_transport();
    EXPECT_NE(ice, nullptr);
    
    // KcpSession 在连接前不存在
    auto kcp = controller->get_kcp_session();
    EXPECT_EQ(kcp, nullptr);
}

TEST_F(PeerControllerTest, SyncTimeoutTimerManagement) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 设置同步超时定时器
    controller->sync_timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    controller->sync_timeout_timer->expires_after(std::chrono::seconds(30));
    
    // close() 应该取消定时器
    controller->close();
    
    EXPECT_EQ(controller->sync_timeout_timer, nullptr);
}

// ═══════════════════════════════════════════════════════════════
// 7. 线程安全测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, ConcurrentStateQueries) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;
    
    // 多个线程并发查询状态
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&controller, &running]() {
            while (running) {
                controller->get_state();
                controller->is_connected();
                controller->is_valid();
                controller->get_connection_type();
                controller->get_kcp_wait_send();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    
    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    running = false;
    for (auto& t : threads) {
        t.join();
    }
    
    // 验证没有崩溃
    EXPECT_TRUE(controller->is_valid());
}

TEST_F(PeerControllerTest, ConcurrentCloseAndQueries) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    std::atomic<bool> running{true};
    
    // 查询线程
    std::thread query_thread([&controller, &running]() {
        while (running) {
            controller->get_state();
            controller->is_valid();
            controller->get_connection_type();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    
    // 短暂延迟后关闭
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    controller->close();
    
    running = false;
    query_thread.join();
    
    // 验证状态正确
    EXPECT_FALSE(controller->is_valid());
}

// ═══════════════════════════════════════════════════════════════
// 8. 原子变量测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, AtomicConnectedAtTs) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 测试原子操作
    int64_t test_ts = 1234567890;
    controller->connected_at_ts.store(test_ts);
    
    EXPECT_EQ(controller->connected_at_ts.load(), test_ts);
}

// ═══════════════════════════════════════════════════════════════
// 9. 边界条件测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, EmptyPeerIds) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // 空的 peer ID 应该仍能创建（虽然不推荐）
    auto controller = PeerController::create(
        "", "", m_io_context, create_ice_config(), std::move(callbacks));
    
    // 可能创建成功也可能失败，取决于 IceTransport 的行为
    // 这里只验证不会崩溃
}

TEST_F(PeerControllerTest, SameSelfAndPeerIds) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // 相同的 self 和 peer ID
    auto controller = PeerController::create(
        "same_id", "same_id", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 这种情况下，is_offer_side 应该是 false（因为 "same_id" < "same_id" 是 false）
    EXPECT_FALSE(controller->is_offer_side());
}

// ═══════════════════════════════════════════════════════════════
// 10. 回调测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, EmptyCallbacks) {
    PeerControllerCallbacks callbacks;
    // 不设置任何回调
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 即使没有回调，操作也不应该崩溃
    controller->initiate_connection();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    controller->close();
}

TEST_F(PeerControllerTest, CallbacksArePostedToIoContext) {
    std::atomic<std::thread::id> callback_thread_id{};
    std::atomic<int> callback_count{0};
    
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [&](PeerState) {
        callback_thread_id = std::this_thread::get_id();
        callback_count++;
    };
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 发起连接，这会触发状态变化
    controller->initiate_connection();
    
    // 等待回调执行
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 回调应该在 io_context 线程中执行
    // 由于 initiate_connection 会改变状态，可能会触发回调
    // 我们只验证回调线程 ID 是 io_context 线程
    if (callback_count > 0) {
        EXPECT_EQ(callback_thread_id.load(), m_io_thread.get_id());
    }
}

// ═══════════════════════════════════════════════════════════════
// 11. KCP 更新测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, UpdateKcpWithoutConnection) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 未连接时调用 update_kcp 不应崩溃
    controller->update_kcp(get_current_ms());
    controller->update_kcp(get_current_ms() + 100);
    controller->update_kcp(get_current_ms() + 200);
    
    EXPECT_TRUE(controller->is_valid());
}

TEST_F(PeerControllerTest, UpdateKcpRapidCalls) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 快速连续调用 update_kcp
    for (int i = 0; i < 100; ++i) {
        controller->update_kcp(get_current_ms());
    }
    
    EXPECT_TRUE(controller->is_valid());
    EXPECT_EQ(controller->get_kcp_wait_send(), 0);
}

// ═══════════════════════════════════════════════════════════════
// 12. 多次 close 测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, MultipleCloseCallsAreSafe) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    EXPECT_TRUE(controller->is_valid());
    
    // 多次调用 close 不应崩溃
    controller->close();
    EXPECT_FALSE(controller->is_valid());
    
    controller->close();
    EXPECT_FALSE(controller->is_valid());
    
    controller->close();
    EXPECT_FALSE(controller->is_valid());
}

TEST_F(PeerControllerTest, CloseAfterInitiateConnection) {
    std::atomic<int> state_change_count{0};
    
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [&state_change_count](PeerState) {
        state_change_count++;
    };
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 发起连接后立即关闭
    controller->initiate_connection();
    controller->close();
    
    EXPECT_FALSE(controller->is_valid());
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
}

// ═══════════════════════════════════════════════════════════════
// 13. 操作顺序测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, OperationsAfterClose) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    controller->close();
    
    // 关闭后的操作应该是安全的（无操作或返回错误）
    controller->initiate_connection();  // 应该无效
    controller->handle_signaling("test", "data");  // 应该无效
    int result = controller->send_message(std::string("test"));
    EXPECT_EQ(result, -1);
    controller->update_kcp(get_current_ms());
    
    EXPECT_FALSE(controller->is_valid());
}

TEST_F(PeerControllerTest, SendMessageOnClosedController) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    controller->close();
    
    // 发送各种大小的消息
    EXPECT_EQ(controller->send_message(std::string("short")), -1);
    EXPECT_EQ(controller->send_message(std::string(1000, 'x')), -1);
    EXPECT_EQ(controller->send_message(std::string(10000, 'y')), -1);
}

// ═══════════════════════════════════════════════════════════════
// 14. 状态一致性测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, StateConsistencyAfterOperations) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 初始状态
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
    EXPECT_TRUE(controller->is_valid());
    EXPECT_FALSE(controller->is_connected());
    
    // 发起连接
    controller->initiate_connection();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 连接中状态
    EXPECT_TRUE(controller->is_valid());
    EXPECT_EQ(controller->get_state(), PeerState::Connecting);
    
    // 关闭
    controller->close();
    
    // 关闭后状态
    EXPECT_FALSE(controller->is_valid());
    EXPECT_FALSE(controller->is_connected());
    EXPECT_EQ(controller->get_state(), PeerState::Disconnected);
}

// ═══════════════════════════════════════════════════════════════
// 15. Conv ID 验证测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, ConvIdWithVariousIds) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // 测试各种 ID 组合
    std::vector<std::pair<std::string, std::string>> test_cases = {
        {"a", "b"},
        {"user1", "user2"},
        {"12345", "67890"},
        {"very_long_peer_id_12345", "another_long_peer_id_67890"},
        {"中文ID", "英文ID"},
    };
    
    for (const auto& [self_id, peer_id] : test_cases) {
        auto ctrl_a = PeerController::create(
            self_id, peer_id, m_io_context, create_ice_config(), callbacks);
        auto ctrl_b = PeerController::create(
            peer_id, self_id, m_io_context, create_ice_config(), callbacks);
        
        ASSERT_NE(ctrl_a, nullptr);
        ASSERT_NE(ctrl_b, nullptr);
        
        // 验证角色相反
        EXPECT_NE(ctrl_a->is_offer_side(), ctrl_b->is_offer_side());
        
        // 清理
        ctrl_a->close();
        ctrl_b->close();
    }
}

// ═══════════════════════════════════════════════════════════════
// 16. 压力测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, RapidCreateAndDestroy) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    // 快速创建和销毁多个 controller
    for (int i = 0; i < 50; ++i) {
        auto controller = PeerController::create(
            "self_" + std::to_string(i),
            "peer_" + std::to_string(i),
            m_io_context,
            create_ice_config(),
            callbacks
        );
        
        ASSERT_NE(controller, nullptr);
        controller->close();
    }
}

TEST_F(PeerControllerTest, ConcurrentCreateCloseAndQuery) {
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i, &success_count, &error_count]() {
            try {
                PeerControllerCallbacks callbacks;
                callbacks.on_state_changed = [](PeerState) {};
                callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
                
                auto controller = PeerController::create(
                    "self_" + std::to_string(i),
                    "peer_" + std::to_string(i),
                    m_io_context,
                    create_ice_config(),
                    std::move(callbacks)
                );
                
                if (controller) {
                    // 执行一些操作
                    controller->get_state();
                    controller->is_valid();
                    controller->get_kcp_wait_send();
                    controller->initiate_connection();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    controller->close();
                    success_count++;
                }
            } catch (...) {
                error_count++;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(error_count.load(), 0);
    EXPECT_EQ(success_count.load(), 10);
}

// ═══════════════════════════════════════════════════════════════
// 17. 同步会话计数测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, SyncSessionCounters) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 测试原子计数器
    controller->sync_session_id.store(12345);
    EXPECT_EQ(controller->sync_session_id.load(), 12345);
    
    controller->expected_file_count.store(100);
    controller->expected_dir_count.store(20);
    EXPECT_EQ(controller->expected_file_count.load(), 100);
    EXPECT_EQ(controller->expected_dir_count.load(), 20);
    
    // 测试 fetch_add
    controller->received_file_count.store(0);
    controller->received_file_count.fetch_add(1);
    controller->received_file_count.fetch_add(5);
    EXPECT_EQ(controller->received_file_count.load(), 6);
    
    controller->received_dir_count.store(0);
    controller->received_dir_count.fetch_add(1);
    controller->received_dir_count.fetch_add(2);
    EXPECT_EQ(controller->received_dir_count.load(), 3);
}

TEST_F(PeerControllerTest, ConcurrentCounterIncrements) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    controller->received_file_count.store(0);
    
    // 多线程并发增加计数
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&controller]() {
            for (int j = 0; j < 100; ++j) {
                controller->received_file_count.fetch_add(1);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(controller->received_file_count.load(), 1000);
}

// ═══════════════════════════════════════════════════════════════
// 18. connected_at_ts 测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, ConnectedAtTimestamp) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 初始连接时间应该是 0
    EXPECT_EQ(controller->connected_at_ts.load(), 0);
    
    // 设置连接时间
    auto now = std::chrono::system_clock::now();
    int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    controller->connected_at_ts.store(ts);
    EXPECT_EQ(controller->connected_at_ts.load(), ts);
}

// ═══════════════════════════════════════════════════════════════
// 19. sync_timeout_timer 测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, SyncTimeoutTimerInitiallyNull) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 初始时定时器应该是 nullptr
    EXPECT_EQ(controller->sync_timeout_timer, nullptr);
}

TEST_F(PeerControllerTest, SyncTimeoutTimerAssignment) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    
    // 可以创建和赋值定时器
    controller->sync_timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    EXPECT_NE(controller->sync_timeout_timer, nullptr);
    
    // 可以设置超时
    controller->sync_timeout_timer->expires_after(std::chrono::seconds(60));
    
    // 可以取消
    controller->sync_timeout_timer->cancel();
    
    // 可以重置
    controller->sync_timeout_timer.reset();
    EXPECT_EQ(controller->sync_timeout_timer, nullptr);
}

// ═══════════════════════════════════════════════════════════════
// 20. 极端 ID 测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, VeryLongIds) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    std::string long_self_id(256, 'a');
    std::string long_peer_id(256, 'b');
    
    auto controller = PeerController::create(
        long_self_id, long_peer_id, m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    EXPECT_EQ(controller->get_self_id(), long_self_id);
    EXPECT_EQ(controller->get_peer_id(), long_peer_id);
}

TEST_F(PeerControllerTest, UnicodeIds) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    std::string unicode_self = "用户一";
    std::string unicode_peer = "用户二";
    
    auto controller = PeerController::create(
        unicode_self, unicode_peer, m_io_context, create_ice_config(), std::move(callbacks));
    
    ASSERT_NE(controller, nullptr);
    EXPECT_EQ(controller->get_self_id(), unicode_self);
    EXPECT_EQ(controller->get_peer_id(), unicode_peer);
}

// ═══════════════════════════════════════════════════════════════
// 21. IceConfig 边界测试
// ═══════════════════════════════════════════════════════════════

TEST_F(PeerControllerTest, EmptyIceConfig) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    IceConfig empty_config;  // 全空配置
    
    // 空配置可能导致创建失败，但不应崩溃
    auto controller = PeerController::create(
        "self", "peer", m_io_context, empty_config, std::move(callbacks));
    
    // 可能返回 nullptr，但绝不能崩溃
    // 如果创建成功，应该是有效的
    if (controller) {
        EXPECT_TRUE(controller->is_valid());
    }
}

TEST_F(PeerControllerTest, IceConfigWithTurn) {
    PeerControllerCallbacks callbacks;
    callbacks.on_state_changed = [](PeerState) {};
    callbacks.on_signal_needed = [](const std::string&, const std::string&) {};
    
    IceConfig config;
    config.stun_host = "stun.l.google.com";
    config.stun_port = 19302;
    config.turn_host = "turn.example.com";
    config.turn_port = 3478;
    config.turn_username = "user";
    config.turn_password = "pass";
    
    auto controller = PeerController::create(
        "self", "peer", m_io_context, config, std::move(callbacks));
    
    // 即使 TURN 服务器不可达，创建也应该成功
    ASSERT_NE(controller, nullptr);
    EXPECT_TRUE(controller->is_valid());
}
