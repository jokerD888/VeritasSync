#include "test_helpers.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/sync/SyncSession.h"

using namespace VeritasSync;
using json = nlohmann::json;

REGISTER_VERITAS_TEST_ENV();

// ============================================================================
// 测试辅助：Mock PeerController
// ============================================================================

// 由于 PeerController 构造复杂（需要 ICE/KCP 等），我们通过创建一个简化的
// mock 场景来测试 SyncSession 的消息处理逻辑

// ============================================================================
// SyncSession 构造和基本功能
// ============================================================================

class SyncSessionBasicTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;
    std::thread io_thread;

    // 捕获发送的消息
    std::vector<std::string> sent_messages;
    std::mutex sent_mutex;

    SyncSession::SendToPeerFunc capture_send;
    SyncSession::WithPeerFunc noop_with_peer;
    SyncSession::GetPeerFunc noop_get_peer;

    void SetUp() override {
        work_guard.emplace(boost::asio::make_work_guard(io_ctx));
        io_thread = std::thread([this]() { io_ctx.run(); });

        capture_send = [this](const std::string& msg, PeerController*) {
            std::lock_guard lk(sent_mutex);
            sent_messages.push_back(msg);
        };

        noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
        noop_get_peer = [](const std::string&) -> std::shared_ptr<PeerController> { return nullptr; };
    }

    void TearDown() override {
        work_guard.reset();
        io_ctx.stop();
        if (io_thread.joinable()) io_thread.join();
    }
};

TEST_F(SyncSessionBasicTest, Construction) {
    SyncSession session(nullptr, worker_pool, io_ctx,
                        capture_send, noop_with_peer, noop_get_peer);
    // 不崩溃即通过
}

TEST_F(SyncSessionBasicTest, SetRole_SetMode) {
    SyncSession session(nullptr, worker_pool, io_ctx,
                        capture_send, noop_with_peer, noop_get_peer);

    session.set_role(SyncRole::Source);
    session.set_mode(SyncMode::BiDirectional);
    // 不崩溃即通过
}

TEST_F(SyncSessionBasicTest, SendSyncBegin_NullPeer_NoOp) {
    SyncSession session(nullptr, worker_pool, io_ctx,
                        capture_send, noop_with_peer, noop_get_peer);

    // 传入 nullptr peer，应直接返回
    session.send_sync_begin(nullptr, 12345, 10, 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::lock_guard lk(sent_mutex);
    EXPECT_TRUE(sent_messages.empty()) << "Should not send when peer is nullptr";
}

TEST_F(SyncSessionBasicTest, SendSyncAck_NullPeer_NoOp) {
    SyncSession session(nullptr, worker_pool, io_ctx,
                        capture_send, noop_with_peer, noop_get_peer);

    session.send_sync_ack(nullptr, 12345, 10, 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::lock_guard lk(sent_mutex);
    EXPECT_TRUE(sent_messages.empty());
}

// ============================================================================
// SyncSession 消息解析测试
// ============================================================================

class SyncSessionMessageTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;
    std::thread io_thread;

    std::vector<std::string> sent_messages;
    std::mutex sent_mutex;

    void SetUp() override {
        work_guard.emplace(boost::asio::make_work_guard(io_ctx));
        io_thread = std::thread([this]() { io_ctx.run(); });
    }

    void TearDown() override {
        work_guard.reset();
        io_ctx.stop();
        if (io_thread.joinable()) io_thread.join();
    }
};

TEST_F(SyncSessionMessageTest, HandleSyncBegin_NullPeer) {
    SyncSession::SendToPeerFunc send = [this](const std::string& msg, PeerController*) {
        std::lock_guard lk(sent_mutex);
        sent_messages.push_back(msg);
    };
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    // nullptr peer 应提前返回
    json payload = {{"session_id", 12345}, {"file_count", 10}, {"dir_count", 5}};
    session.handle_sync_begin(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncSessionMessageTest, HandleSyncBegin_MalformedPayload) {
    SyncSession::SendToPeerFunc send = [this](const std::string& msg, PeerController*) {
        std::lock_guard lk(sent_mutex);
        sent_messages.push_back(msg);
    };
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    // 缺少必要字段的 payload
    json bad_payload = {{"wrong_field", 42}};
    // 应该被 try-catch 捕获，不崩溃
    session.handle_sync_begin(bad_payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncSessionMessageTest, HandleSyncAck_NullPeer) {
    SyncSession::SendToPeerFunc send = [this](const std::string& msg, PeerController*) {
        std::lock_guard lk(sent_mutex);
        sent_messages.push_back(msg);
    };
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    json payload = {{"session_id", 12345}, {"received_files", 8}, {"received_dirs", 3}};
    session.handle_sync_ack(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncSessionMessageTest, HandleSyncAck_MalformedPayload) {
    SyncSession::SendToPeerFunc send = [this](const std::string& msg, PeerController*) {
        std::lock_guard lk(sent_mutex);
        sent_messages.push_back(msg);
    };
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    json bad_payload = {{"wrong", "data"}};
    // 应该被 try-catch 捕获
    session.handle_sync_ack(bad_payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// SyncSession perform_flood_sync 测试
// ============================================================================

class SyncSessionFloodSyncTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;
    std::thread io_thread;

    void SetUp() override {
        work_guard.emplace(boost::asio::make_work_guard(io_ctx));
        io_thread = std::thread([this]() { io_ctx.run(); });
    }

    void TearDown() override {
        work_guard.reset();
        io_ctx.stop();
        if (io_thread.joinable()) io_thread.join();
    }
};

TEST_F(SyncSessionFloodSyncTest, NullController_Returns) {
    SyncSession::SendToPeerFunc send = [](const std::string&, PeerController*) {};
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    // 传入 nullptr controller，应直接返回
    session.perform_flood_sync(nullptr, 12345);
    // 不崩溃即通过
}

TEST_F(SyncSessionFloodSyncTest, NullStateManager_Returns) {
    SyncSession::SendToPeerFunc send = [](const std::string&, PeerController*) {};
    SyncSession::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};
    SyncSession::GetPeerFunc get_peer = [](const std::string&) -> std::shared_ptr<PeerController> {
        return nullptr;
    };

    // StateManager 为 nullptr
    SyncSession session(nullptr, worker_pool, io_ctx, send, with_peer, get_peer);

    // 即使 controller 不为 nullptr，StateManager 为空也应返回
    // 但这里由于无法简单创建 PeerController，我们仍传 nullptr
    session.perform_flood_sync(nullptr, 12345);
}

// ============================================================================
// SyncSession 协议消息格式验证
// ============================================================================

TEST(SyncSessionProtocol, SyncBegin_MessageFormat) {
    // 验证 sync_begin 消息的 JSON 格式符合协议规范
    json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_BEGIN;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", 12345},
        {"file_count", 100},
        {"dir_count", 20}
    };

    EXPECT_EQ(msg[Protocol::MSG_TYPE].get<std::string>(), "sync_begin");
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["session_id"].get<uint64_t>(), 12345);
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["file_count"].get<size_t>(), 100);
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["dir_count"].get<size_t>(), 20);
}

TEST(SyncSessionProtocol, SyncAck_MessageFormat) {
    json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", 12345},
        {"received_files", 80},
        {"received_dirs", 15}
    };

    EXPECT_EQ(msg[Protocol::MSG_TYPE].get<std::string>(), "sync_ack");
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["session_id"].get<uint64_t>(), 12345);
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["received_files"].get<size_t>(), 80);
    EXPECT_EQ(msg[Protocol::MSG_PAYLOAD]["received_dirs"].get<size_t>(), 15);
}

TEST(SyncSessionProtocol, AllProtocolTypes_NotEmpty) {
    // 验证所有协议类型常量不为空
    EXPECT_NE(std::string(Protocol::MSG_TYPE), "");
    EXPECT_NE(std::string(Protocol::MSG_PAYLOAD), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_BEGIN), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_ACK), "");
    EXPECT_NE(std::string(Protocol::TYPE_SYNC_COMPLETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_UPDATE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_DELETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_UPDATE_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_FILE_DELETE_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_BATCH), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_CREATE), "");
    EXPECT_NE(std::string(Protocol::TYPE_DIR_DELETE), "");
    EXPECT_NE(std::string(Protocol::TYPE_HEARTBEAT), "");
    EXPECT_NE(std::string(Protocol::TYPE_HEARTBEAT_ACK), "");
    EXPECT_NE(std::string(Protocol::TYPE_GOODBYE), "");
}
