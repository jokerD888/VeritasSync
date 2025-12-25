// tests/test_kcp_session.cpp
// 测试 KCP 会话的基础功能

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <queue>
#include <mutex>

#include "VeritasSync/net/KcpSession.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// 全局测试环境
class KcpTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const kcp_env =
    ::testing::AddGlobalTestEnvironment(new KcpTestEnvironment());

/**
 * @brief 模拟网络传输的管道
 * 用于连接两个 KcpSession 进行本地测试
 */
class MockNetworkPipe {
public:
    void send_to_peer(const char* data, int len) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffer.emplace(std::string(data, len));
    }
    
    std::vector<std::string> receive_all() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> result;
        while (!m_buffer.empty()) {
            result.push_back(std::move(m_buffer.front()));
            m_buffer.pop();
        }
        return result;
    }
    
private:
    std::queue<std::string> m_buffer;
    std::mutex m_mutex;
};

class KcpSessionTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
    
    uint32_t get_current_ms() {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
};

// --- 创建和销毁测试 ---

TEST_F(KcpSessionTest, CreateSession) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    callbacks.on_message_received = [](const std::string&) {};
    
    auto session = KcpSession::create(12345, std::move(callbacks));
    
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->get_conv(), 12345);
    EXPECT_TRUE(session->is_valid());
}

TEST_F(KcpSessionTest, SessionWithCustomConfig) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    
    KcpConfig config;
    config.nodelay = 1;
    config.interval = 10;
    config.resend = 2;
    config.nc = 1;
    config.snd_wnd = 1024;
    config.rcv_wnd = 1024;
    
    auto session = KcpSession::create(67890, std::move(callbacks), config);
    
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->is_valid());
}

// --- 发送测试 ---

TEST_F(KcpSessionTest, SendMessage) {
    std::vector<std::string> output_data;
    
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [&output_data](const char* data, int len) {
        output_data.emplace_back(data, len);
        return 0;
    };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    ASSERT_NE(session, nullptr);
    
    // 发送消息
    std::string message = "Hello, KCP!";
    int result = session->send(message);
    EXPECT_EQ(result, 0);
    
    // 驱动 KCP 状态机
    session->update(get_current_ms());
    
    // 应该有输出数据
    EXPECT_FALSE(output_data.empty());
}

TEST_F(KcpSessionTest, SendEmptyMessage) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    
    std::string empty;
    int result = session->send(empty);
    
    // 发送空消息应该成功或返回错误码（取决于 KCP 实现）
    EXPECT_TRUE(result == 0 || result < 0);
}

TEST_F(KcpSessionTest, SendLargeMessage) {
    std::vector<std::string> output_data;
    
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [&output_data](const char* data, int len) {
        output_data.emplace_back(data, len);
        return 0;
    };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    
    // 发送大消息 (64KB)
    std::string large_message(65536, 'X');
    int result = session->send(large_message);
    EXPECT_EQ(result, 0);
    
    // 驱动 KCP
    session->update(get_current_ms());
    
    // 大消息会被切片，应该有多个输出包
    EXPECT_GT(output_data.size(), 1);
}

// --- 流控测试 ---

TEST_F(KcpSessionTest, WaitSendCount) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    
    // 初始应该没有等待发送的数据
    EXPECT_EQ(session->get_wait_send_count(), 0);
    
    // 发送一些消息
    for (int i = 0; i < 10; ++i) {
        session->send("Message " + std::to_string(i));
    }
    
    // 应该有等待发送的数据
    EXPECT_GT(session->get_wait_send_count(), 0);
    
    // 驱动 KCP 发送
    for (int i = 0; i < 100; ++i) {
        session->update(get_current_ms());
    }
}

// --- 双向通信测试 ---

TEST_F(KcpSessionTest, TwoWayCommunication) {
    MockNetworkPipe pipe_a_to_b;
    MockNetworkPipe pipe_b_to_a;
    
    std::vector<std::string> received_by_a;
    std::vector<std::string> received_by_b;
    
    // 创建会话 A
    KcpSessionCallbacks callbacks_a;
    callbacks_a.on_output = [&pipe_a_to_b](const char* data, int len) {
        pipe_a_to_b.send_to_peer(data, len);
        return 0;
    };
    callbacks_a.on_message_received = [&received_by_a](const std::string& msg) {
        received_by_a.push_back(msg);
    };
    
    // 创建会话 B（相同的 conv）
    KcpSessionCallbacks callbacks_b;
    callbacks_b.on_output = [&pipe_b_to_a](const char* data, int len) {
        pipe_b_to_a.send_to_peer(data, len);
        return 0;
    };
    callbacks_b.on_message_received = [&received_by_b](const std::string& msg) {
        received_by_b.push_back(msg);
    };
    
    auto session_a = KcpSession::create(1001, std::move(callbacks_a));
    auto session_b = KcpSession::create(1001, std::move(callbacks_b));
    
    ASSERT_NE(session_a, nullptr);
    ASSERT_NE(session_b, nullptr);
    
    // A 发送消息给 B
    session_a->send(std::string("Hello from A"));
    
    // B 发送消息给 A
    session_b->send(std::string("Hello from B"));
    
    // 模拟网络传输和 KCP 更新循环
    for (int i = 0; i < 50; ++i) {
        uint32_t now = get_current_ms();
        
        session_a->update(now);
        session_b->update(now);
        
        // A -> B 的数据
        for (const auto& pkt : pipe_a_to_b.receive_all()) {
            session_b->input(pkt.data(), pkt.size());
        }
        
        // B -> A 的数据
        for (const auto& pkt : pipe_b_to_a.receive_all()) {
            session_a->input(pkt.data(), pkt.size());
        }
        
        // 接收消息
        session_a->receive();
        session_b->receive();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    // 验证消息传递
    ASSERT_GE(received_by_b.size(), 1);
    EXPECT_EQ(received_by_b[0], "Hello from A");
    
    ASSERT_GE(received_by_a.size(), 1);
    EXPECT_EQ(received_by_a[0], "Hello from B");
}

// --- 消息顺序测试 ---

TEST_F(KcpSessionTest, MessageOrderPreserved) {
    MockNetworkPipe pipe;
    std::vector<std::string> received_messages;
    
    KcpSessionCallbacks sender_cb;
    sender_cb.on_output = [&pipe](const char* data, int len) {
        pipe.send_to_peer(data, len);
        return 0;
    };
    
    KcpSessionCallbacks receiver_cb;
    receiver_cb.on_output = [](const char*, int) { return 0; };
    receiver_cb.on_message_received = [&received_messages](const std::string& msg) {
        received_messages.push_back(msg);
    };
    
    auto sender = KcpSession::create(2001, std::move(sender_cb));
    auto receiver = KcpSession::create(2001, std::move(receiver_cb));
    
    // 发送多条消息
    for (int i = 0; i < 10; ++i) {
        sender->send("Message " + std::to_string(i));
    }
    
    // 模拟传输
    for (int tick = 0; tick < 100; ++tick) {
        uint32_t now = get_current_ms();
        sender->update(now);
        receiver->update(now);
        
        for (const auto& pkt : pipe.receive_all()) {
            receiver->input(pkt.data(), pkt.size());
        }
        
        receiver->receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    
    // 验证消息顺序
    ASSERT_EQ(received_messages.size(), 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(received_messages[i], "Message " + std::to_string(i));
    }
}

// --- 边界情况测试 ---

TEST_F(KcpSessionTest, InputInvalidData) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    
    // 输入无效数据应该不崩溃
    std::string garbage = "random garbage data";
    int result = session->input(garbage.data(), garbage.size());
    
    // 结果应该是错误码
    EXPECT_NE(result, 0);
}

TEST_F(KcpSessionTest, ReceiveWithoutInput) {
    KcpSessionCallbacks callbacks;
    callbacks.on_output = [](const char*, int) { return 0; };
    
    auto session = KcpSession::create(1, std::move(callbacks));
    
    // 没有输入数据时接收应该返回空
    auto messages = session->receive();
    EXPECT_TRUE(messages.empty());
}
