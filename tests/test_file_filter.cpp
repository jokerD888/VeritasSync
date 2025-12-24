// tests/test_file_filter.cpp
// 测试文件过滤器的规则匹配正确性

#include <gtest/gtest.h>
#include <string>
#include <filesystem>
#include <fstream>

#include "VeritasSync/storage/FileFilter.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

// 全局测试环境
class FileFilterTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        init_logger();
    }
};

static ::testing::Environment* const filter_env =
    ::testing::AddGlobalTestEnvironment(new FileFilterTestEnvironment());

class FileFilterTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir = std::filesystem::path("test_filter_dir");
    std::filesystem::path ignore_file;
    
    void SetUp() override {
        // 创建测试目录
        std::filesystem::create_directories(test_dir);
        ignore_file = test_dir / ".veritasignore";
    }
    
    void TearDown() override {
        // 清理测试目录
        std::filesystem::remove_all(test_dir);
    }
    
    void write_ignore_file(const std::string& content) {
        std::ofstream f(ignore_file);
        f << content;
        f.close();
    }
};

// --- 基础过滤测试（匹配实际的默认规则）---

TEST_F(FileFilterTest, DefaultRulesIgnoreGitDir) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_TRUE(filter.should_ignore(".git/config"));
    EXPECT_TRUE(filter.should_ignore("subdir/.git"));
}

TEST_F(FileFilterTest, DefaultRulesIgnoreVeritasDB) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 实际默认规则使用 .veritas.db
    EXPECT_TRUE(filter.should_ignore(".veritas.db"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-journal"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-wal"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-shm"));
    EXPECT_TRUE(filter.should_ignore("subdir/.veritas.db"));
}

TEST_F(FileFilterTest, DefaultRulesIgnoreVeritasIgnore) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore(".veritasignore"));
    EXPECT_TRUE(filter.should_ignore("subdir/.veritasignore"));
}

TEST_F(FileFilterTest, DefaultRulesIgnorePartFiles) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // *.part 模式
    EXPECT_TRUE(filter.should_ignore("download.part"));
    EXPECT_TRUE(filter.should_ignore("file.part"));
    EXPECT_TRUE(filter.should_ignore("subdir/file.part"));
}

TEST_F(FileFilterTest, DefaultRulesIgnoreSystemFiles) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore(".DS_Store"));
    EXPECT_TRUE(filter.should_ignore("Thumbs.db"));
    EXPECT_TRUE(filter.should_ignore("subdir/.DS_Store"));
}

TEST_F(FileFilterTest, DefaultRulesIgnoreBuildDirs) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 目录模式 build/, bin/, out/
    EXPECT_TRUE(filter.should_ignore("build"));
    EXPECT_TRUE(filter.should_ignore("build/output.exe"));
    EXPECT_TRUE(filter.should_ignore("bin"));
    EXPECT_TRUE(filter.should_ignore("bin/app.exe"));
    EXPECT_TRUE(filter.should_ignore("out"));
    EXPECT_TRUE(filter.should_ignore("out/result.txt"));
}

TEST_F(FileFilterTest, NormalFilesNotIgnored) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_FALSE(filter.should_ignore("document.txt"));
    EXPECT_FALSE(filter.should_ignore("image.png"));
    EXPECT_FALSE(filter.should_ignore("src/main.cpp"));
}

// --- 自定义规则测试 ---

TEST_F(FileFilterTest, CustomGlobPattern) {
    write_ignore_file("*.log\n*.tmp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("app.log"));
    EXPECT_TRUE(filter.should_ignore("debug.log"));
    EXPECT_TRUE(filter.should_ignore("temp.tmp"));
    EXPECT_FALSE(filter.should_ignore("app.txt"));
}

TEST_F(FileFilterTest, CustomDirectoryPattern) {
    write_ignore_file("node_modules/\ncache/\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("node_modules"));
    EXPECT_TRUE(filter.should_ignore("node_modules/package/index.js"));
    EXPECT_TRUE(filter.should_ignore("cache"));
}

TEST_F(FileFilterTest, CustomExactFileName) {
    write_ignore_file("secret.key\npassword.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("secret.key"));
    EXPECT_TRUE(filter.should_ignore("subdir/secret.key"));
    EXPECT_TRUE(filter.should_ignore("password.txt"));
}

TEST_F(FileFilterTest, CommentLinesIgnored) {
    write_ignore_file("# This is a comment\n*.log\n# Another comment\n*.tmp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("file.log"));
    EXPECT_TRUE(filter.should_ignore("file.tmp"));
}

TEST_F(FileFilterTest, EmptyLinesIgnored) {
    write_ignore_file("*.log\n\n\n*.tmp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("file.log"));
    EXPECT_TRUE(filter.should_ignore("file.tmp"));
}

// --- 复杂模式测试 ---

TEST_F(FileFilterTest, ExtensionPattern) {
    write_ignore_file("*.pyc\n*.pyo\n__pycache__/\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("module.pyc"));
    EXPECT_TRUE(filter.should_ignore("module.pyo"));
    EXPECT_TRUE(filter.should_ignore("__pycache__"));
}

// --- 边界情况测试 ---

TEST_F(FileFilterTest, EmptyPath) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 空路径应该不被忽略（或安全处理）
    EXPECT_FALSE(filter.should_ignore(""));
}

TEST_F(FileFilterTest, PathWithSpaces) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_FALSE(filter.should_ignore("path with spaces/file name.txt"));
}

TEST_F(FileFilterTest, HiddenFilesNotIgnoredByDefault) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 默认不应该忽略所有隐藏文件
    EXPECT_FALSE(filter.should_ignore(".bashrc"));
    EXPECT_FALSE(filter.should_ignore(".config"));
}

TEST_F(FileFilterTest, NoIgnoreFile) {
    // 删除忽略文件，只使用默认规则
    std::filesystem::remove(ignore_file);
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 默认规则仍应工作
    EXPECT_TRUE(filter.should_ignore(".veritas.db"));
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_FALSE(filter.should_ignore("normal.txt"));
}

// --- 性能相关测试 ---

TEST_F(FileFilterTest, ManyRules) {
    // 写入大量规则
    std::string content;
    for (int i = 0; i < 100; ++i) {
        content += "pattern_" + std::to_string(i) + ".bak\n";
    }
    write_ignore_file(content);
    
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 验证规则正常加载
    EXPECT_TRUE(filter.should_ignore("pattern_50.bak"));
    EXPECT_FALSE(filter.should_ignore("pattern_50.txt"));
}

TEST_F(FileFilterTest, DeepNestedPath) {
    FileFilter filter;
    filter.load_rules(test_dir);
    
    std::string deep_path;
    for (int i = 0; i < 20; ++i) {
        deep_path += "level" + std::to_string(i) + "/";
    }
    deep_path += "file.txt";
    
    EXPECT_FALSE(filter.should_ignore(deep_path));
}
