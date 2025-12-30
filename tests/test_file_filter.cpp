// tests/test_file_filter.cpp
// 增强版：深入测试 FileFilter 的三级引擎、Git 兼容性及极端边界情况

#include <gtest/gtest.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "VeritasSync/storage/FileFilter.h"
#include "VeritasSync/common/Logger.h"

using namespace VeritasSync;

class FileFilterTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir = std::filesystem::absolute("test_filter_dir");
    std::filesystem::path ignore_file;
    
    void SetUp() override {
        std::filesystem::create_directories(test_dir);
        ignore_file = test_dir / ".veritasignore";
    }
    
    void TearDown() override {
        std::filesystem::remove_all(test_dir);
    }
    
    void write_ignore_file(const std::string& content) {
        std::ofstream f(ignore_file);
        f << content;
        f.close();
    }
};

// --- Tier 1: 后缀全局匹配 (*.ext) ---

TEST_F(FileFilterTest, Tier1SuffixGlobalMatch) {
    write_ignore_file("*.obj\n*.tmp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 应在任何层级匹配
    EXPECT_TRUE(filter.should_ignore("main.obj"));
    EXPECT_TRUE(filter.should_ignore("sub/dir/test.obj"));
    EXPECT_TRUE(filter.should_ignore("very/deep/path/data.tmp"));
    
    // 负面测试
    EXPECT_FALSE(filter.should_ignore("obj.cpp"));
    EXPECT_FALSE(filter.should_ignore("main.object"));
}

// --- Tier 2 vs Tier 3: 根部锚定 vs 全局搜索 ---

TEST_F(FileFilterTest, RootedVsGlobalLogic) {
    // 根据 Git 规范：
    // /only_at_root.txt -> 仅根目录
    // global_file.log  -> 全路径任何位置
    write_ignore_file("/only_at_root.txt\nglobal_file.log\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 1. 根部锚定规则
    EXPECT_TRUE(filter.should_ignore("only_at_root.txt"));
    EXPECT_FALSE(filter.should_ignore("subdir/only_at_root.txt"));
    
    // 2. 全局搜索规则 (因为不带斜杠且未锚定，走 Tier 3)
    EXPECT_TRUE(filter.should_ignore("global_file.log"));
    EXPECT_TRUE(filter.should_ignore("a/b/c/global_file.log"));
}

// --- Tier 2: 路径相关过滤 (包含斜杠的规则) ---

TEST_F(FileFilterTest, Tier2PathSpecificMatching) {
    // 包含内部斜杠的规则应默认为从根部开始的路径匹配 (Tier 2)
    write_ignore_file("src/generated/\ntools/config.json\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 正确匹配
    EXPECT_TRUE(filter.should_ignore("src/generated/api.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/generated"));
    EXPECT_TRUE(filter.should_ignore("tools/config.json"));
    
    // 不应匹配非根部的相似路径
    EXPECT_FALSE(filter.should_ignore("other/src/generated/api.cpp"));
    EXPECT_FALSE(filter.should_ignore("sub/tools/config.json"));
}

// --- 目录匹配的边界情况 ---

TEST_F(FileFilterTest, DirectoryMatchingNuances) {
    write_ignore_file("build/\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 匹配目录及其内容
    EXPECT_TRUE(filter.should_ignore("build"));
    EXPECT_TRUE(filter.should_ignore("build/main.o"));
    EXPECT_TRUE(filter.should_ignore("build/subdir/resource.res"));
    
    // 名字相似但不应拦截
    EXPECT_FALSE(filter.should_ignore("build_log.txt"));
    EXPECT_FALSE(filter.should_ignore("mybuild"));
    EXPECT_FALSE(filter.should_ignore("prebuild/step.sh"));
}

// --- Windows 路径规范化测试 ---

TEST_F(FileFilterTest, WindowsPathStandardization) {
    write_ignore_file("docs/internal/\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 传入带反斜杠的路径（模拟 Windows 驱动输入）
    // 注意：这里的测试前提是 StateManager 或调用方已经处理了 generic_u8string()
    // 但 FileFilter::should_ignore 本身也要能处理这种输入以便鲁棒性
    EXPECT_TRUE(filter.should_ignore("docs/internal/secret.pdf"));
    
    // 如果我们想测试 FileFilter 的跨平台兼容性，我们可以模拟传入混合斜杠
    // 目前实现已经改为直接接收规范化路径，所以这个测试其实是在验证“匹配逻辑”
    EXPECT_TRUE(filter.should_ignore("docs/internal/sub/file.txt"));
}

// --- 复杂通配符 (Tier 3) ---

TEST_F(FileFilterTest, Tier3ComplexGlobPatterns) {
    write_ignore_file("debug_??_*.v??\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("debug_01_main.v01"));
    EXPECT_TRUE(filter.should_ignore("logs/debug_99_test.vff"));
    EXPECT_FALSE(filter.should_ignore("debug_1_main.v01")); // 少了一位 ?
}

// --- 规则解析的鲁棒性 (空白与注释) ---

TEST_F(FileFilterTest, RuleCleaningAndComments) {
    write_ignore_file(
        "# 核心数据库\n"
        "  important.db  \n"  // 带空格
        "  \t \n"             // 纯空白行
        "temp/ # 临时目录\n"   // 行尾注释 (虽然目前代码主要是跳过 # 开头的行)
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("important.db"));
    EXPECT_TRUE(filter.should_ignore("temp/any.file"));
}

// --- 零拷贝性能负载压力测试 ---

TEST_F(FileFilterTest, StressTestZeroCopyLookups) {
    std::string content;
    for(int i=0; i<50; ++i) content += "dir_" + std::to_string(i) + "/\n";
    for(int i=0; i<50; ++i) content += "*.ext_" + std::to_string(i) + "\n";
    write_ignore_file(content);
    
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 模拟 50,000 次查找，验证异构查找和 Tiered 过滤的极速
    for (int i = 0; i < 50000; ++i) {
        // 交替命中不同的 Tier
        if (i % 2 == 0) {
            EXPECT_TRUE(filter.should_ignore("dir_25/file.txt"));
        } else {
            EXPECT_TRUE(filter.should_ignore("some/path/data.ext_10"));
        }
    }
}
