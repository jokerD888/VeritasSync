// tests/test_reconnect_heartbeat.cpp
// ⑤ ReconnectPolicy 和 HeartbeatService 子组件单元测试

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "VeritasSync/p2p/ReconnectPolicy.h"
#include "VeritasSync/p2p/HeartbeatService.h"
#include "VeritasSync/p2p/PeerController.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

// ═══════════════════════════════════════════════════════════════
// ReconnectPolicy 测试
// ═══════════════════════════════════════════════════════════════

class ReconnectPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_io_context = std::make_unique<boost::asio::io_context>();
        m_work_guard = std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(m_io_context->get_executor());

        m_io_thread = std::thread([this]() {
            m_io_context->run();
        });
    }

    void TearDown() override {
        m_work_guard.reset();
        m_io_context->stop();
        if (m_io_thread.joinable()) {
            m_io_thread.join();
        }
    }

    std::unique_ptr<boost::asio::io_context> m_io_context;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> m_work_guard;
    std::thread m_io_thread;
};

// --- 基本属性测试 ---

TEST_F(ReconnectPolicyTest, DefaultParameters) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {}
    );

    EXPECT_EQ(policy->get_max_attempts(), ReconnectPolicy::DEFAULT_MAX_ATTEMPTS);
    EXPECT_EQ(policy->get_base_delay_ms(), ReconnectPolicy::DEFAULT_BASE_DELAY_MS);
    EXPECT_EQ(policy->get_max_attempts(), 5);
    EXPECT_EQ(policy->get_base_delay_ms(), 3000);
}

TEST_F(ReconnectPolicyTest, CustomParameters) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {},
        10,   // max_attempts
        500   // base_delay_ms
    );

    EXPECT_EQ(policy->get_max_attempts(), 10);
    EXPECT_EQ(policy->get_base_delay_ms(), 500);
}

// --- 重试计数测试 ---

TEST_F(ReconnectPolicyTest, InitialAttemptsIsZero) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {}
    );

    EXPECT_EQ(policy->get_attempts("peer_a"), 0);
    EXPECT_EQ(policy->get_attempts("peer_b"), 0);
}

TEST_F(ReconnectPolicyTest, ScheduleIncreasesAttempts) {
    // 使用很大的 delay 避免回调真正触发
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {},
        5,       // max_attempts
        100000   // 100s delay，避免在测试期间触发
    );

    policy->schedule_reconnect("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 1);

    policy->schedule_reconnect("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 2);

    policy->schedule_reconnect("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 3);

    // 其他 peer 不受影响
    EXPECT_EQ(policy->get_attempts("peer_b"), 0);
}

TEST_F(ReconnectPolicyTest, ClearAttemptsResetsCount) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {},
        5, 100000
    );

    policy->schedule_reconnect("peer_a");
    policy->schedule_reconnect("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 2);

    policy->clear_attempts("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 0);
}

TEST_F(ReconnectPolicyTest, ClearNonexistentPeerIsSafe) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {}
    );

    // 清除不存在的 peer 不应崩溃
    policy->clear_attempts("nonexistent");
    EXPECT_EQ(policy->get_attempts("nonexistent"), 0);
}

// --- 超过最大重试次数测试 ---

TEST_F(ReconnectPolicyTest, ExceedMaxAttemptsStopsReconnect) {
    std::atomic<int> reconnect_calls{0};

    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [&reconnect_calls](const std::string&) { reconnect_calls++; },
        [](const std::string&) { return false; },
        [](const std::string&) {},
        3,       // 只允许 3 次
        100000   // 大 delay
    );

    policy->schedule_reconnect("peer_a");  // 1
    EXPECT_EQ(policy->get_attempts("peer_a"), 1);
    policy->schedule_reconnect("peer_a");  // 2
    EXPECT_EQ(policy->get_attempts("peer_a"), 2);
    policy->schedule_reconnect("peer_a");  // 3
    EXPECT_EQ(policy->get_attempts("peer_a"), 3);

    // 第 4 次应被拒绝（超过上限），attempts 被清除
    policy->schedule_reconnect("peer_a");
    EXPECT_EQ(policy->get_attempts("peer_a"), 0);
}

// --- 实际重连回调触发测试 ---

TEST_F(ReconnectPolicyTest, ReconnectCallbackFired) {
    std::atomic<int> reconnect_calls{0};
    std::atomic<int> cleanup_calls{0};
    std::string reconnect_peer_id;
    std::mutex mtx;

    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [&](const std::string& peer_id) {
            std::lock_guard<std::mutex> lock(mtx);
            reconnect_peer_id = peer_id;
            reconnect_calls++;
        },
        [](const std::string&) { return false; },  // 始终未连接
        [&cleanup_calls](const std::string&) { cleanup_calls++; },
        5,
        50  // 50ms 很快触发
    );

    policy->schedule_reconnect("peer_x");

    // 等待回调触发
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_GE(reconnect_calls.load(), 1);
    EXPECT_GE(cleanup_calls.load(), 1);
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(reconnect_peer_id, "peer_x");
    }
}

// --- 已重连时取消重连测试 ---

TEST_F(ReconnectPolicyTest, SkipReconnectIfAlreadyConnected) {
    std::atomic<int> reconnect_calls{0};
    std::atomic<int> cleanup_calls{0};

    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [&reconnect_calls](const std::string&) { reconnect_calls++; },
        [](const std::string&) { return true; },   // 始终已连接
        [&cleanup_calls](const std::string&) { cleanup_calls++; },
        5,
        50  // 50ms
    );

    policy->schedule_reconnect("peer_y");

    // 等待定时器触发
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 应该跳过重连（已连接）
    EXPECT_EQ(reconnect_calls.load(), 0);
    EXPECT_EQ(cleanup_calls.load(), 0);
}

// --- 指数退避延迟测试 ---

TEST_F(ReconnectPolicyTest, ExponentialBackoffDelay) {
    // 通过测量回调触发时间来验证指数退避
    std::vector<std::chrono::steady_clock::time_point> trigger_times;
    std::mutex mtx;

    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [&](const std::string&) {
            std::lock_guard<std::mutex> lock(mtx);
            trigger_times.push_back(std::chrono::steady_clock::now());
        },
        [](const std::string&) { return false; },
        [](const std::string&) {},
        3,
        100  // 基础 100ms，第二次 200ms，第三次 400ms
    );

    auto start = std::chrono::steady_clock::now();

    // 连续调度 3 次（模拟三次失败后重试）
    policy->schedule_reconnect("peer_z");  // 100ms 后触发

    // 等待第一次触发
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_GE(trigger_times.size(), 1u);

        // 第一次应在约 100ms 后触发（容忍 50ms 误差）
        auto delay_1 = std::chrono::duration_cast<std::chrono::milliseconds>(
            trigger_times[0] - start).count();
        EXPECT_GE(delay_1, 80);
        EXPECT_LE(delay_1, 300);
    }
}

// --- 多 peer 独立计数测试 ---

TEST_F(ReconnectPolicyTest, MultiplePeersIndependent) {
    auto policy = std::make_unique<ReconnectPolicy>(
        *m_io_context,
        [](const std::string&) {},
        [](const std::string&) { return false; },
        [](const std::string&) {},
        5, 100000
    );

    policy->schedule_reconnect("alice");
    policy->schedule_reconnect("alice");
    policy->schedule_reconnect("bob");

    EXPECT_EQ(policy->get_attempts("alice"), 2);
    EXPECT_EQ(policy->get_attempts("bob"), 1);
    EXPECT_EQ(policy->get_attempts("charlie"), 0);

    policy->clear_attempts("alice");
    EXPECT_EQ(policy->get_attempts("alice"), 0);
    EXPECT_EQ(policy->get_attempts("bob"), 1);
}

// ═══════════════════════════════════════════════════════════════
// HeartbeatService 测试
// ═══════════════════════════════════════════════════════════════

class HeartbeatServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_io_context = std::make_unique<boost::asio::io_context>();
        m_work_guard = std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(m_io_context->get_executor());

        m_io_thread = std::thread([this]() {
            m_io_context->run();
        });
    }

    void TearDown() override {
        m_work_guard.reset();
        m_io_context->stop();
        if (m_io_thread.joinable()) {
            m_io_thread.join();
        }
    }

    std::unique_ptr<boost::asio::io_context> m_io_context;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> m_work_guard;
    std::thread m_io_thread;
};

// --- 基本属性测试 ---

TEST_F(HeartbeatServiceTest, DefaultInterval) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {}
    );

    EXPECT_EQ(service->get_interval_ms(), HeartbeatService::DEFAULT_HEARTBEAT_INTERVAL_MS);
    EXPECT_EQ(service->get_interval_ms(), 20000);
}

TEST_F(HeartbeatServiceTest, CustomInterval) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {},
        5000
    );

    EXPECT_EQ(service->get_interval_ms(), 5000);
}

TEST_F(HeartbeatServiceTest, SetInterval) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {}
    );

    service->set_interval_ms(10000);
    EXPECT_EQ(service->get_interval_ms(), 10000);

    service->set_interval_ms(1000);
    EXPECT_EQ(service->get_interval_ms(), 1000);
}

// --- 启停测试 ---

TEST_F(HeartbeatServiceTest, StartAndStop) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {},
        100  // 短间隔便于测试
    );

    // 启停不应崩溃
    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    service->stop();
}

TEST_F(HeartbeatServiceTest, DoubleStartIsSafe) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {},
        100
    );

    service->start();
    service->start();  // 二次启动不应崩溃
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    service->stop();
}

TEST_F(HeartbeatServiceTest, DoubleStopIsSafe) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {},
        100
    );

    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    service->stop();
    service->stop();  // 二次停止不应崩溃
}

TEST_F(HeartbeatServiceTest, StopWithoutStartIsSafe) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {}
    );

    // 未启动就停止不应崩溃
    service->stop();
}

// --- 心跳发送定时触发测试 ---

TEST_F(HeartbeatServiceTest, NoPeers_NoSendCalls) {
    std::atomic<int> send_calls{0};

    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },  // 空 peer 列表
        [&send_calls](const std::string&, PeerController*) { send_calls++; },
        50  // 50ms 间隔
    );

    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    service->stop();

    // 没有 peer 时不应有 send 调用
    EXPECT_EQ(send_calls.load(), 0);
}

// --- handle_heartbeat / handle_heartbeat_ack 空指针安全 ---

TEST_F(HeartbeatServiceTest, HandleHeartbeatNullPeer) {
    std::atomic<int> send_calls{0};

    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [&send_calls](const std::string&, PeerController*) { send_calls++; }
    );

    // nullptr 不应崩溃
    service->handle_heartbeat(nullptr);
    EXPECT_EQ(send_calls.load(), 0);
}

TEST_F(HeartbeatServiceTest, HandleHeartbeatAckNullPeer) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {}
    );

    // nullptr 不应崩溃
    service->handle_heartbeat_ack(nullptr);
}

// --- 销毁安全测试 ---

TEST_F(HeartbeatServiceTest, DestroyWhileRunning) {
    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        []() { return std::vector<std::shared_ptr<PeerController>>{}; },
        [](const std::string&, PeerController*) {},
        50
    );

    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 运行期间销毁不应崩溃
    service.reset();
}

// --- collect_peers 回调异常安全测试 ---

TEST_F(HeartbeatServiceTest, CollectPeersReturnsEmpty) {
    int collect_calls = 0;

    auto service = std::make_unique<HeartbeatService>(
        *m_io_context,
        [&collect_calls]() {
            collect_calls++;
            return std::vector<std::shared_ptr<PeerController>>{};
        },
        [](const std::string&, PeerController*) {},
        50
    );

    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    service->stop();

    // collect_peers 至少被调用了一次
    EXPECT_GE(collect_calls, 1);
}
