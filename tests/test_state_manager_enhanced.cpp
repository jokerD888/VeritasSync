// tests/test_state_manager_enhanced.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/p2p/P2PManager.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// 简单的 MockP2PManager，只满足 StateManager 的构造需求
class MinimalMockP2P : public P2PManager {
public:
    MinimalMockP2P() : P2PManager() {} // 调用 Protected 构造函数
    
    boost::asio::io_context& get_io_context() override { return m_ctx; }
    
    // 捕获广播，以便验证
    void broadcast_file_update(const FileInfo& info) override {
        m_broadcast_count++;
    }

    std::atomic<int> m_broadcast_count{0};
private:
    boost::asio::io_context m_ctx;
};

class StateManagerEnhancedTest : public ::testing::Test {
protected:
    std::filesystem::path test_root = "test_state_root";
    std::unique_ptr<MinimalMockP2P> mock_p2p;
    std::unique_ptr<StateManager> sm;

    // --- 授权包装器 ---
    // 由于 TEST_F 会生成子类，子类无法直接利用父类的友元权限，
    // 我们必须通过这些在 Fixture 中定义的 Protected 函数来中转。
    void trigger_notify(const std::string& p) {
        sm->notify_change_detected(p);
    }
    
    void trigger_process() {
        sm->process_debounced_changes();
    }

    void SetUp() override {
        if (std::filesystem::exists(test_root)) {
            std::filesystem::remove_all(test_root);
        }
        std::filesystem::create_directories(test_root);
        
        mock_p2p = std::make_unique<MinimalMockP2P>();
        sm = std::make_unique<StateManager>(test_root.string(), *mock_p2p, true, "test_sync");
    }

    void TearDown() override {
        // 1. 显式释放 StateManager，触发其内部 Database 和 FileWatcher 的析构
        sm.reset();
        mock_p2p.reset();

        // 2. 给 Windows 系统一点点缓冲时间来真正释放文件句柄 (50ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 3. 健壮的清理 logic
        if (std::filesystem::exists(test_root)) {
            std::error_code ec;
            // 尝试多次清理，因为有时删除数据库后，WAL文件还需要一点时间消失
            for (int i = 0; i < 5; ++i) {
                std::filesystem::remove_all(test_root, ec);
                if (!ec) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    void create_test_file(const std::string& rel_path, const std::string& content) {
        auto full_path = test_root / rel_path;
        std::filesystem::create_directories(full_path.parent_path());
        std::ofstream of(full_path);
        of << content;
        of.close();
    }
};

// 1. 测试 子树修剪 (Subtree Pruning)
TEST_F(StateManagerEnhancedTest, SubtreePruning) {
    create_test_file("keep/file.txt", "keep me");
    create_test_file("ignore_dir/sub/hidden.txt", "hide me");
    
    create_test_file(".veritasignore", "ignore_dir/\n");
    sm->scan_directory();

    auto files = sm->get_all_files();
    bool found_hidden = false;
    for (const auto& f : files) {
        if (f.path.find("ignore_dir") != std::string::npos) found_hidden = true;
    }
    EXPECT_FALSE(found_hidden);
}

// 2. 测试 规则重载触发器
TEST_F(StateManagerEnhancedTest, RuleReloadOnIgnoreFileChange) {
    create_test_file("temp.log", "some log");
    sm->scan_directory();
    EXPECT_NE(sm->get_file_hash("temp.log"), "");

    create_test_file(".veritasignore", "*.log\n");
    
    // 使用包装器触发私有逻辑
    auto ignore_path = std::filesystem::absolute(test_root / ".veritasignore").string();
    trigger_notify(ignore_path);
    trigger_process();

    // 再次扫描看结果
    sm->scan_directory();
    EXPECT_EQ(sm->get_file_hash("temp.log"), "");
}

// 3. 测试 数据库僵尸记录清理
TEST_F(StateManagerEnhancedTest, CleanupDatabaseZombies) {
    create_test_file("kill_me.txt", "data");
    sm->scan_directory();
    
    std::filesystem::remove(test_root / "kill_me.txt");
    sm->scan_directory();
    
    EXPECT_EQ(sm->get_file_hash("kill_me.txt"), "");
}

} // namespace VeritasSync
