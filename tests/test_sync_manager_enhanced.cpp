#include <gtest/gtest.h>

#include <optional>
#include <vector>
#include <stdexcept>

#include "VeritasSync/sync/SyncManager.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// 全局测试环境：初始化 Logger
class SyncManagerEnhancedTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

// 注册全局环境
static ::testing::Environment* const enhanced_env =
    ::testing::AddGlobalTestEnvironment(new SyncManagerEnhancedTestEnvironment());

// 辅助函数：创建 FileInfo
FileInfo make_file(const std::string& path, const std::string& hash, uint64_t mtime = 1000) {
    return {path, hash, mtime};
}

// ============================================================================
// 新增测试：路径大小写不敏感
// ============================================================================

TEST(SyncManagerEnhanced, PathCaseInsensitivity_SameFile) {
    // 测试：README.txt vs readme.TXT 应该被识别为同一文件
    std::vector<FileInfo> local = {make_file("README.txt", "hash123")};
    std::vector<FileInfo> remote = {make_file("readme.TXT", "hash123")};
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, 
        [](auto) { return std::nullopt; },
        SyncMode::OneWay
    );
    
    // 应该识别为相同文件，无需操作
    EXPECT_TRUE(actions.files_to_request.empty()) << "应该识别为同一文件（Hash相同）";
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncManagerEnhanced, PathCaseInsensitivity_DifferentHash) {
    // 测试：大小写不同但Hash不同，应该请求更新
    std::vector<FileInfo> local = {make_file("Docs/README.txt", "hash_old")};
    std::vector<FileInfo> remote = {make_file("docs/readme.TXT", "hash_new")};
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote,
        [](auto) { return std::nullopt; },
        SyncMode::OneWay
    );
    
    // 应该识别为同一文件，但Hash不同，需要更新
    ASSERT_EQ(actions.files_to_request.size(), 1) << "应该请求更新";
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncManagerEnhanced, PathCaseInsensitivity_MixedCase) {
    // 测试：多个文件，混合大小写
    std::vector<FileInfo> local = {
        make_file("SRC/Main.cpp", "hash1"),
        make_file("docs/guide.md", "hash2")
    };
    std::vector<FileInfo> remote = {
        make_file("src/MAIN.CPP", "hash1"),  // 大小写不同
        make_file("DOCS/guide.md", "hash2")  // 大小写不同
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote,
        [](auto) { return std::nullopt; },
        SyncMode::OneWay
    );
    
    EXPECT_TRUE(actions.files_to_request.empty()) << "大小写不同但Hash相同，无需操作";
    EXPECT_TRUE(actions.files_to_delete.empty());
}

// ============================================================================
// 新增测试：目录路径大小写不敏感
// ============================================================================

TEST(SyncManagerEnhanced, DirCaseInsensitivity_SameDir) {
    std::set<std::string> local_dirs = {"Docs", "SRC/Utils"};
    std::set<std::string> remote_dirs = {"docs", "src/utils"};  // 小写
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::OneWay);
    
    // 应该识别为相同目录
    EXPECT_TRUE(actions.dirs_to_create.empty()) << "大小写不同的目录应该被识别为相同";
    EXPECT_TRUE(actions.dirs_to_delete.empty());
}

TEST(SyncManagerEnhanced, DirCaseInsensitivity_NewDir) {
    std::set<std::string> local_dirs = {"Docs"};
    std::set<std::string> remote_dirs = {"docs", "NEW_DIR"};  // 有新目录
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::OneWay);
    
    // Docs 和 docs 应该被识别为相同，只有 NEW_DIR 是新的
    ASSERT_EQ(actions.dirs_to_create.size(), 1);
    EXPECT_EQ(actions.dirs_to_create[0], "NEW_DIR");
}

// ============================================================================
// 新增测试：异常处理
// ============================================================================

TEST(SyncManagerEnhanced, ExceptionHandling_DatabaseError) {
    // 测试：get_history 抛出异常时的处理
    std::vector<FileInfo> local = {
        make_file("good1.txt", "hash1"),
        make_file("bad_db_entry.txt", "hash2"),
        make_file("good2.txt", "hash3")
    };
    std::vector<FileInfo> remote;
    
    // Mock：第二个文件会抛出异常
    auto throwing_history = [](const std::string& path) -> std::optional<SyncHistory> {
        if (path == "bad_db_entry.txt") {
            throw std::runtime_error("Database corruption");
        }
        return std::nullopt;
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, throwing_history, SyncMode::BiDirectional
    );
    
    // 关键：即使有异常，也应该处理完所有文件
    // bad_db_entry.txt 的异常被捕获，返回 nullopt，所以被当作"本地新增"保留
    EXPECT_TRUE(actions.files_to_delete.empty()) << "异常不应中断同步，所有文件应被保留";
}

TEST(SyncManagerEnhanced, ExceptionHandling_MultipleErrors) {
    // 测试：多个文件都抛出异常
    std::vector<FileInfo> local = {
        make_file("error1.txt", "hash1"),
        make_file("error2.txt", "hash2"),
        make_file("error3.txt", "hash3")
    };
    std::vector<FileInfo> remote;
    
    auto always_throw = [](const std::string& path) -> std::optional<SyncHistory> {
        throw std::runtime_error("Persistent DB error");
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, always_throw, SyncMode::BiDirectional
    );
    
    // 所有异常都应该被捕获，文件应该被保留
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncManagerEnhanced, ExceptionHandling_UnknownException) {
    // 测试：非 std::exception 的异常
    std::vector<FileInfo> local = {make_file("strange.txt", "hash1")};
    std::vector<FileInfo> remote;
    
    auto throw_unknown = [](const std::string& path) -> std::optional<SyncHistory> {
        throw 42;  // 奇怪的异常
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, throw_unknown, SyncMode::BiDirectional
    );
    
    // 未知异常也应该被捕获
    EXPECT_TRUE(actions.files_to_delete.empty());
}

// ============================================================================
// 新增测试：冲突检测（三向合并）
// ============================================================================

TEST(SyncManagerEnhanced, ConflictDetection_OfflineNewFile) {
    // 场景：双方都离线新建了同名文件
    std::vector<FileInfo> local = {make_file("conflict.txt", "hash_A")};
    std::vector<FileInfo> remote = {make_file("conflict.txt", "hash_B")};
    
    // 无历史记录
    auto no_history = [](auto) { return std::nullopt; };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, no_history, SyncMode::BiDirectional
    );
    
    // 应该检测到冲突
    ASSERT_EQ(actions.files_to_conflict_rename.size(), 1) << "应该检测到离线新建冲突";
    EXPECT_EQ(actions.files_to_conflict_rename[0], "conflict.txt");
    
    // 也应该请求下载远程版本
    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], "conflict.txt");
}

TEST(SyncManagerEnhanced, ConflictDetection_ConcurrentModification) {
    // 场景：基于旧版本的并发修改
    std::vector<FileInfo> local = {make_file("modified.txt", "hash_local_edit")};
    std::vector<FileInfo> remote = {make_file("modified.txt", "hash_remote_edit")};
    
    // 有历史记录，但与双方都不同
    auto history_base = [](const std::string& path) -> std::optional<SyncHistory> {
        return SyncHistory{"hash_base_version", 1000};
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, history_base, SyncMode::BiDirectional
    );
    
    // 应该检测到冲突（本地修改了，远程也修改了）
    ASSERT_EQ(actions.files_to_conflict_rename.size(), 1) << "应该检测到并发修改冲突";
    EXPECT_EQ(actions.files_to_conflict_rename[0], "modified.txt");
}

TEST(SyncManagerEnhanced, ConflictDetection_OnlyRemoteModified) {
    // 场景：只有远程修改了，本地未修改
    std::vector<FileInfo> local = {make_file("updated.txt", "hash_base")};
    std::vector<FileInfo> remote = {make_file("updated.txt", "hash_new")};
    
    // 历史记录与本地相同
    auto history_matches_local = [](const std::string& path) -> std::optional<SyncHistory> {
        return SyncHistory{"hash_base", 1000};
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, history_matches_local, SyncMode::BiDirectional
    );
    
    // 不应该是冲突，只是正常更新
    EXPECT_TRUE(actions.files_to_conflict_rename.empty()) << "只远程修改不应是冲突";
    
    // 应该请求下载
    ASSERT_EQ(actions.files_to_request.size(), 1);
}

// ============================================================================
// 新增测试：输入验证
// ============================================================================

TEST(SyncManagerEnhanced, InputValidation_NullHistoryCallback) {
    // 测试：nullptr 回调应该被拒绝
    std::vector<FileInfo> local = {make_file("test.txt", "hash1")};
    std::vector<FileInfo> remote;
    
    SyncManager::HistoryQueryFunc null_callback = nullptr;
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, null_callback, SyncMode::OneWay
    );
    
    // 应该返回空结果
    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
    EXPECT_TRUE(actions.files_to_conflict_rename.empty());
}

// ============================================================================
// 新增测试：边界情况
// ============================================================================

TEST(SyncManagerEnhanced, BoundaryCase_VeryLongPath) {
    // 测试：非常长的路径
    std::string long_path = "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z/"
                           "very_long_file_name_that_might_be_near_path_limit.txt";
    
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {make_file(long_path, "hash1")};
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    ASSERT_EQ(actions.files_to_request.size(), 1);
    EXPECT_EQ(actions.files_to_request[0], long_path);
}

TEST(SyncManagerEnhanced, BoundaryCase_EmptyPath) {
    // 测试：空路径（虽然不应该出现，但要测试健壮性）
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {make_file("", "hash1")};
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    // 应该能处理（即使路径为空）
    EXPECT_EQ(actions.files_to_request.size(), 1);
}

TEST(SyncManagerEnhanced, BoundaryCase_DuplicatePaths) {
    // 测试：输入中有重复路径（虽然不应该，但要测试）
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote = {
        make_file("dup.txt", "hash1"),
        make_file("dup.txt", "hash2")  // 重复路径
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    // 应该能处理（可能保留最后一个）
    EXPECT_GE(actions.files_to_request.size(), 1);
}

// ============================================================================
// 新增测试：性能相关（大数据量）
// ============================================================================

TEST(SyncManagerEnhanced, Performance_10kFiles) {
    std::vector<FileInfo> local;
    std::vector<FileInfo> remote;
    
    // 创建 10,000 个文件
    for (int i = 0; i < 10000; ++i) {
        remote.push_back(make_file(
            "dir" + std::to_string(i / 100) + "/file_" + std::to_string(i) + ".txt",
            "hash_" + std::to_string(i)
        ));
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_EQ(actions.files_to_request.size(), 10000);
    
    // 性能要求：10k 文件应该在 100ms 内完成
    EXPECT_LT(duration.count(), 100) 
        << "10k 文件处理耗时: " << duration.count() << " ms (应该 < 100ms)";
}

TEST(SyncManagerEnhanced, Performance_10kDirs) {
    std::set<std::string> local_dirs;
    std::set<std::string> remote_dirs;
    
    // 创建 10,000 个目录
    for (int i = 0; i < 10000; ++i) {
        remote_dirs.insert("dir" + std::to_string(i));
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    auto actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, SyncMode::OneWay);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    EXPECT_EQ(actions.dirs_to_create.size(), 10000);
    
    // 性能要求
    EXPECT_LT(duration.count(), 50) 
        << "10k 目录处理耗时: " << duration.count() << " ms (应该 < 50ms)";
}

// ============================================================================
// 新增测试：路径归一化的边界情况
// ============================================================================

TEST(SyncManagerEnhanced, PathNormalization_TrailingSlash) {
    // 测试：尾部斜杠应该被忽略
    std::vector<FileInfo> local = {make_file("dir/file.txt", "hash1")};
    std::vector<FileInfo> remote = {make_file("dir/file.txt/", "hash1")};  // 尾部斜杠
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    // 应该识别为同一文件
    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
}

TEST(SyncManagerEnhanced, PathNormalization_BackslashVsForwardSlash) {
    // 测试：路径分隔符统一
    std::vector<FileInfo> local = {make_file("dir\\file.txt", "hash1")};  // Windows 风格
    std::vector<FileInfo> remote = {make_file("dir/file.txt", "hash1")};  // Unix 风格
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, [](auto) { return std::nullopt; }, SyncMode::OneWay
    );
    
    // 应该识别为同一文件
    EXPECT_TRUE(actions.files_to_request.empty());
    EXPECT_TRUE(actions.files_to_delete.empty());
}

// ============================================================================
// 新增测试：双向模式的复杂场景
// ============================================================================

TEST(SyncManagerEnhanced, BiDirectional_MixedOperations) {
    // 复杂场景：混合新增、删除、冲突
    std::vector<FileInfo> local = {
        make_file("local_new.txt", "hash_ln"),      // 本地新增
        make_file("deleted_remote.txt", "hash_dr"), // 远程删除
        make_file("conflict.txt", "hash_lc")        // 冲突
    };
    std::vector<FileInfo> remote = {
        make_file("remote_new.txt", "hash_rn"),     // 远程新增
        make_file("conflict.txt", "hash_rc")        // 冲突
    };
    
    auto history = [](const std::string& path) -> std::optional<SyncHistory> {
        if (path == "local_new.txt") return std::nullopt;  // 无历史
        if (path == "deleted_remote.txt") return SyncHistory{"hash_dr", 1000};  // 历史匹配
        if (path == "conflict.txt") return SyncHistory{"hash_base", 1000};  // 冲突
        return std::nullopt;
    };
    
    auto actions = SyncManager::compare_states_and_get_requests(
        local, remote, history, SyncMode::BiDirectional
    );
    
    // 本地新增：保留
    EXPECT_EQ(std::count(actions.files_to_delete.begin(), actions.files_to_delete.end(), "local_new.txt"), 0);
    
    // 远程删除（历史匹配）：删除
    EXPECT_EQ(std::count(actions.files_to_delete.begin(), actions.files_to_delete.end(), "deleted_remote.txt"), 1);
    
    // 冲突：重命名 + 下载
    EXPECT_EQ(std::count(actions.files_to_conflict_rename.begin(), actions.files_to_conflict_rename.end(), "conflict.txt"), 1);
    
    // 远程新增 + 冲突文件：请求下载
    EXPECT_GE(actions.files_to_request.size(), 2);
}
