#include "test_helpers.h"
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Config.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/sync/SyncHandler.h"
#include "VeritasSync/sync/TransferManager.h"

using namespace VeritasSync;
using json = nlohmann::json;

REGISTER_VERITAS_TEST_ENV();

// ============================================================================
// SyncHandler 角色/模式测试
// ============================================================================

class SyncHandlerRoleTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;

    // 空回调 (仅用于测试角色/模式判断，不需要实际发送消息)
    SyncHandler::SendToPeerFunc noop_send = [](const std::string&, PeerController*) {};
    SyncHandler::SendToPeerSafeFunc noop_send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    std::shared_ptr<TransferManager> make_dummy_transfer_manager() {
        // TransferManager 需要一个 SendCallback
        return std::make_shared<TransferManager>(
            nullptr, io_ctx, worker_pool,
            [](const std::string&, const std::string&) -> int { return 0; }
        );
    }
};

TEST_F(SyncHandlerRoleTest, Construction_DefaultRole) {
    auto tm = make_dummy_transfer_manager();
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);

    // 默认应该是 Source 角色（基于头文件默认值）
    // Source + OneWay 模式下，can_receive() 应返回 false
    // 通过 handle_file_update 传入 nullptr peer 来间接验证

    // handle_file_update 在 Source + OneWay 模式下应该直接 return
    json payload = {{"path", "test.txt"}, {"hash", "abc"}, {"mtime", 1000}, {"size", 100}};
    // 不应崩溃
    handler.handle_file_update(payload, nullptr);
}

TEST_F(SyncHandlerRoleTest, SetRole_Destination_CanReceive) {
    auto tm = make_dummy_transfer_manager();
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);

    handler.set_role(SyncRole::Destination);
    handler.set_mode(SyncMode::OneWay);

    // Destination + OneWay: 应该可以接收
    // handle_file_update 应进入处理流程（但因为 StateManager 是 nullptr，会提前返回）
    json payload = {{"path", "test.txt"}, {"hash", "abc"}, {"mtime", 1000}, {"size", 100}};
    handler.handle_file_update(payload, nullptr);
    // 不崩溃即通过
}

TEST_F(SyncHandlerRoleTest, SetRole_Source_BiDirectional_CanReceive) {
    auto tm = make_dummy_transfer_manager();
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);

    handler.set_role(SyncRole::Source);
    handler.set_mode(SyncMode::BiDirectional);

    // Source + BiDirectional: 应该可以接收
    json payload = {{"path", "test.txt"}, {"hash", "abc"}, {"mtime", 1000}, {"size", 100}};
    handler.handle_file_update(payload, nullptr);
}

// ============================================================================
// SyncHandler 消息解析测试
// ============================================================================

class SyncHandlerParsingTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;

    SyncHandler::SendToPeerFunc noop_send = [](const std::string&, PeerController*) {};
    SyncHandler::SendToPeerSafeFunc noop_send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    std::shared_ptr<TransferManager> tm;

    void SetUp() override {
        tm = std::make_shared<TransferManager>(
            nullptr, io_ctx, worker_pool,
            [](const std::string&, const std::string&) -> int { return 0; }
        );
        work_guard.emplace(boost::asio::make_work_guard(io_ctx));
    }

    void TearDown() override {
        work_guard.reset();
        io_ctx.stop();
    }
};

TEST_F(SyncHandlerParsingTest, HandleFileDelete_InvalidPayload_NoStateManager) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 没有 StateManager 应提前返回（不崩溃）
    json payload = {{"path", "deleted.txt"}};
    handler.handle_file_delete(payload, nullptr);

    // 等待 worker 线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerParsingTest, HandleDirDelete_InvalidPayload_LogsError) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 传入缺少 path 字段的 payload，应该被 catch 捕获并记日志
    json bad_payload = {{"wrong_key", "value"}};
    handler.handle_dir_delete(bad_payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerParsingTest, HandleDirCreate_Source_OneWay_Rejected) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Source);
    handler.set_mode(SyncMode::OneWay);

    // Source + OneWay 模式下，dir_create 应被拒绝
    json payload = {{"path", "new_dir"}};
    handler.handle_dir_create(payload, nullptr);
    // 不崩溃即通过
}

// ============================================================================
// SyncHandler 批量消息处理测试
// ============================================================================

class SyncHandlerBatchTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;

    SyncHandler::SendToPeerFunc noop_send = [](const std::string&, PeerController*) {};
    SyncHandler::SendToPeerSafeFunc noop_send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    std::shared_ptr<TransferManager> tm;

    void SetUp() override {
        tm = std::make_shared<TransferManager>(
            nullptr, io_ctx, worker_pool,
            [](const std::string&, const std::string&) -> int { return 0; }
        );
    }
};

TEST_F(SyncHandlerBatchTest, HandleFileUpdateBatch_MissingFilesField) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // payload 没有 "files" 字段，应记录错误并返回
    json payload = {{"wrong", "data"}};
    handler.handle_file_update_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerBatchTest, HandleFileDeleteBatch_MissingPathsField) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // payload 没有 "paths" 字段
    json payload = {{"wrong", "data"}};
    handler.handle_file_delete_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerBatchTest, HandleDirBatch_EmptyBatch) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 空的批量操作应正常处理
    json payload = {
        {"creates", json::array()},
        {"deletes", json::array()}
    };
    handler.handle_dir_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerBatchTest, HandleDirBatch_Source_OneWay_Rejected) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Source);
    handler.set_mode(SyncMode::OneWay);

    json payload = {
        {"creates", {"dir1", "dir2"}},
        {"deletes", {"dir3"}}
    };
    // Source + OneWay 应拒绝处理
    handler.handle_dir_batch(payload, nullptr);
}

TEST_F(SyncHandlerBatchTest, HandleFileUpdateBatch_ValidPayload_NoStateManager) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 有效的 payload，但没有 StateManager，应提前返回
    json payload;
    payload["files"] = json::array();
    payload["files"].push_back({{"path", "file1.txt"}, {"hash", "abc"}, {"mtime", 1000}, {"size", 100}});
    payload["files"].push_back({{"path", "file2.txt"}, {"hash", "def"}, {"mtime", 2000}, {"size", 200}});

    handler.handle_file_update_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// SyncHandler 消息回调验证测试
// ============================================================================

class SyncHandlerCallbackTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;

    std::vector<std::string> sent_messages;
    std::mutex sent_mutex;

    std::shared_ptr<TransferManager> tm;

    void SetUp() override {
        tm = std::make_shared<TransferManager>(
            nullptr, io_ctx, worker_pool,
            [](const std::string&, const std::string&) -> int { return 0; }
        );
    }
};

TEST_F(SyncHandlerCallbackTest, HandleFileRequest_IsPlaceholder) {
    // handle_file_request 目前是占位方法，应该不做任何操作
    SyncHandler::SendToPeerFunc send = [this](const std::string& msg, PeerController*) {
        std::lock_guard lk(sent_mutex);
        sent_messages.push_back(msg);
    };
    SyncHandler::SendToPeerSafeFunc send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        send, send_safe, with_peer);

    json payload = {{"path", "request.txt"}};
    handler.handle_file_request(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::lock_guard lk(sent_mutex);
    EXPECT_TRUE(sent_messages.empty()) << "handle_file_request should be a no-op placeholder";
}

TEST_F(SyncHandlerCallbackTest, SetStateManager_NullSafety) {
    SyncHandler::SendToPeerFunc noop_send = [](const std::string&, PeerController*) {};
    SyncHandler::SendToPeerSafeFunc noop_send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);

    // 设置 nullptr，不应崩溃
    handler.set_state_manager(nullptr);

    handler.set_role(SyncRole::Destination);
    // 任何操作都不应崩溃
    json payload = {{"path", "test.txt"}, {"hash", "abc"}, {"mtime", 1000}, {"size", 100}};
    handler.handle_file_update(payload, nullptr);
    handler.handle_file_delete(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ============================================================================
// SyncHandler 消息解析鲁棒性测试
// ============================================================================

class SyncHandlerRobustnessTest : public ::testing::Test {
protected:
    boost::asio::thread_pool worker_pool{2};
    boost::asio::io_context io_ctx;
    std::shared_ptr<TransferManager> tm;

    SyncHandler::SendToPeerFunc noop_send = [](const std::string&, PeerController*) {};
    SyncHandler::SendToPeerSafeFunc noop_send_safe = [](const std::string&, const std::string&) {};
    SyncHandler::WithPeerFunc noop_with_peer = [](const std::string&, std::function<void(PeerController*)>) {};

    void SetUp() override {
        tm = std::make_shared<TransferManager>(
            nullptr, io_ctx, worker_pool,
            [](const std::string&, const std::string&) -> int { return 0; }
        );
    }
};

TEST_F(SyncHandlerRobustnessTest, HandleShareState_NullPeer) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // payload 合法但 peer 为 nullptr，应该提前返回（peer_id 为空）
    json payload;
    payload["files"] = json::array();
    payload["directories"] = json::array();
    handler.handle_share_state(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerRobustnessTest, HandleDirBatch_MalformedPayload) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 完全错误的 payload 格式
    json payload = 42;  // 不是对象
    handler.handle_dir_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(SyncHandlerRobustnessTest, HandleFileDeleteBatch_ValidPaths) {
    SyncHandler handler(nullptr, tm, worker_pool, io_ctx,
                        noop_send, noop_send_safe, noop_with_peer);
    handler.set_role(SyncRole::Destination);

    // 有效的 paths，但没有 StateManager
    json payload;
    payload["paths"] = {"file1.txt", "dir/file2.txt", "a/b/c/file3.txt"};
    handler.handle_file_delete_batch(payload, nullptr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
