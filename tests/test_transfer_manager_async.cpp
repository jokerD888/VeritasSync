// tests/test_transfer_manager_async.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <atomic>

#include "VeritasSync/sync/TransferManager.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/common/CryptoLayer.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/P2PManager.h"

namespace VeritasSync {

// Mock P2PManager to provide io_context
class MockP2PForTransfer : public P2PManager {
public:
    boost::asio::io_context& get_io_context() override { return m_ioc; }
private:
    boost::asio::io_context m_ioc;
};

class TransferManagerAsyncTest : public ::testing::Test {
protected:
    std::filesystem::path test_root = "test_transfer_root";
    boost::asio::thread_pool worker_pool{4};
    CryptoLayer crypto;
    std::shared_ptr<TransferManager> tm;
    std::unique_ptr<StateManager> sm;
    std::shared_ptr<MockP2PForTransfer> mock_p2p;
    
    std::atomic<int> sent_packets{0};
    std::mutex payload_mutex;

    void SetUp() override {
        if (std::filesystem::exists(test_root)) {
            std::filesystem::remove_all(test_root);
        }
        std::filesystem::create_directories(test_root);
        
        crypto.set_key("test_transfer_key_1234567890123");
        
        mock_p2p = std::make_shared<MockP2PForTransfer>();
        sm = std::make_unique<StateManager>(test_root.string(), *mock_p2p, false, "test_sync");
        
        auto send_cb = [this](const std::string& /*peer_id*/, const std::string& /*data*/) {
            sent_packets++;
            return 0; 
        };
        
        tm = std::make_shared<TransferManager>(sm.get(), worker_pool, send_cb);
    }

    void TearDown() override {
        tm.reset();
        sm.reset();
        mock_p2p.reset();
        worker_pool.join();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::filesystem::exists(test_root)) {
            std::error_code ec;
            std::filesystem::remove_all(test_root, ec);
        }
    }

    void create_dummy_file(const std::string& name, size_t size) {
        std::ofstream of(test_root / name, std::ios::binary);
        std::vector<char> buffer(size, 'A');
        of.write(buffer.data(), buffer.size());
        of.close();
        
        // 我们需要让 StateManager 知道这个文件的存在，否则 get_file_hash 会返回空
        sm->scan_directory();
    }
};

// 1. 测试异步上传队列
TEST_F(TransferManagerAsyncTest, AsyncUploadBasic) {
    create_dummy_file("large.bin", 50000); 
    
    nlohmann::json payload;
    payload["path"] = "large.bin";
    
    tm->queue_upload("peer_A", payload); // 应该传入 payload 字段的内容
    
    int retries = 0;
    while (sent_packets < 4 && retries < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }
    
    EXPECT_GE(sent_packets.load(), 4); 
}

// 2. 测试并发稳定性
TEST_F(TransferManagerAsyncTest, ConcurrentUploadStress) {
    const int FILE_COUNT = 10;
    for(int i=0; i<FILE_COUNT; ++i) {
        create_dummy_file("file_" + std::to_string(i), 1000);
    }
    
    for(int i=0; i<FILE_COUNT; ++i) {
        nlohmann::json payload;
        payload["path"] = "file_" + std::to_string(i);
        tm->queue_upload("peer_" + std::to_string(i), payload);
    }
    
    int retries = 0;
    while (sent_packets < FILE_COUNT && retries < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retries++;
    }
    
    EXPECT_GE(sent_packets.load(), FILE_COUNT);
}

} // namespace VeritasSync
