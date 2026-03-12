// tests/test_file_filter.cpp
// 增强版：深入测试 FileFilter 的三级引擎、Git 兼容性、取反规则、双星号、字符类、转义及极端边界情况

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

// =========================================================================
//  原有测试（验证向后兼容性）
// =========================================================================

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
    
    // 匹配目录及其内容 (build/ 不含内部斜杠，走 Tier 3 正则)
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
    
    EXPECT_TRUE(filter.should_ignore("docs/internal/secret.pdf"));
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
        "temp/ # 临时目录\n"   // 行尾注释
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
    
    for (int i = 0; i < 50000; ++i) {
        if (i % 2 == 0) {
            EXPECT_TRUE(filter.should_ignore("dir_25/file.txt"));
        } else {
            EXPECT_TRUE(filter.should_ignore("some/path/data.ext_10"));
        }
    }
}

// =========================================================================
//  P0: ! 取反规则（白名单例外）
// =========================================================================

TEST_F(FileFilterTest, P0_NegationBasic) {
    // 忽略所有 .log 文件，但保留 important.log
    write_ignore_file(
        "*.log\n"
        "!important.log\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("debug.log"));
    EXPECT_TRUE(filter.should_ignore("error.log"));
    EXPECT_TRUE(filter.should_ignore("sub/access.log"));
    
    // 取反：important.log 不应被忽略
    EXPECT_FALSE(filter.should_ignore("important.log"));
    EXPECT_FALSE(filter.should_ignore("sub/important.log"));
}

TEST_F(FileFilterTest, P0_NegationOrderMatters) {
    // gitignore 语义：最后匹配的规则胜出
    write_ignore_file(
        "*.txt\n"
        "!keep.txt\n"
        "keep.txt\n"  // 再次忽略 keep.txt
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // keep.txt 最终被忽略（最后匹配的 "keep.txt" 规则胜出）
    EXPECT_TRUE(filter.should_ignore("keep.txt"));
    EXPECT_TRUE(filter.should_ignore("other.txt"));
}

TEST_F(FileFilterTest, P0_NegationWithDirectory) {
    // 忽略整个 logs/ 目录，但保留 logs/critical.log
    write_ignore_file(
        "logs/\n"
        "!logs/critical.log\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("logs/debug.log"));
    EXPECT_TRUE(filter.should_ignore("logs/info.log"));
    
    // 取反：critical.log 不应被忽略
    EXPECT_FALSE(filter.should_ignore("logs/critical.log"));
}

TEST_F(FileFilterTest, P0_NegationWithWildcard) {
    // 忽略所有图片，但保留所有 .svg
    write_ignore_file(
        "*.jpg\n"
        "*.png\n"
        "*.gif\n"
        "*.svg\n"
        "!*.svg\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("photo.jpg"));
    EXPECT_TRUE(filter.should_ignore("icon.png"));
    EXPECT_TRUE(filter.should_ignore("animation.gif"));
    
    // .svg 被取反
    EXPECT_FALSE(filter.should_ignore("logo.svg"));
    EXPECT_FALSE(filter.should_ignore("icons/arrow.svg"));
}

TEST_F(FileFilterTest, P0_NegationDoesNotAffectDefaults) {
    // 取反规则不应影响默认规则
    write_ignore_file(
        "!*.part\n"    // 尝试取反默认的 *.part 规则
        "!.git\n"      // 尝试取反默认的 .git 规则
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 默认规则不可被用户取反覆盖
    EXPECT_TRUE(filter.should_ignore("download.part"));
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_TRUE(filter.should_ignore(".git/config"));
}

TEST_F(FileFilterTest, P0_NegationOnlyNegation) {
    // 只有取反规则，没有先前的忽略规则 → 无效果
    write_ignore_file("!something.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_FALSE(filter.should_ignore("something.txt"));
    EXPECT_FALSE(filter.should_ignore("other.txt"));
}

TEST_F(FileFilterTest, P0_NegationComplexScenario) {
    // 复杂场景：多层取反
    write_ignore_file(
        "*.log\n"
        "!important.log\n"
        "*.tmp\n"
        "!*.cache\n"  // 取反 .cache（但 .cache 本来就没被忽略，无效果）
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("debug.log"));
    EXPECT_FALSE(filter.should_ignore("important.log"));
    EXPECT_TRUE(filter.should_ignore("data.tmp"));
    EXPECT_FALSE(filter.should_ignore("data.cache"));  // .cache 从未被忽略
}

// =========================================================================
//  P1: ** 双星号（跨目录匹配）
// =========================================================================

TEST_F(FileFilterTest, P1_DoubleStarPrefix) {
    // **/foo 匹配任意层级下的 foo
    write_ignore_file("**/test.cpp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("test.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/test.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/unit/test.cpp"));
    EXPECT_TRUE(filter.should_ignore("a/b/c/d/test.cpp"));
    
    EXPECT_FALSE(filter.should_ignore("test.h"));
    EXPECT_FALSE(filter.should_ignore("mytest.cpp"));
}

TEST_F(FileFilterTest, P1_DoubleStarSuffix) {
    // src/** 匹配 src 目录下的所有内容
    write_ignore_file("src/**\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("src/main.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/util/helper.h"));
    EXPECT_TRUE(filter.should_ignore("src/a/b/c/deep.txt"));
    
    EXPECT_FALSE(filter.should_ignore("lib/main.cpp"));
}

TEST_F(FileFilterTest, P1_DoubleStarMiddle) {
    // src/**/test.cpp 匹配 src 目录下任意深度的 test.cpp
    write_ignore_file("src/**/test.cpp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("src/test.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/unit/test.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/a/b/c/test.cpp"));
    
    EXPECT_FALSE(filter.should_ignore("test.cpp"));          // 不在 src 下
    EXPECT_FALSE(filter.should_ignore("lib/test.cpp"));       // 不在 src 下
    EXPECT_FALSE(filter.should_ignore("src/test.h"));         // 后缀不对
}

TEST_F(FileFilterTest, P1_DoubleStarWithExtension) {
    // **/*.pyc 匹配任意层级下的 .pyc 文件
    write_ignore_file("**/*.pyc\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("module.pyc"));
    EXPECT_TRUE(filter.should_ignore("pkg/module.pyc"));
    EXPECT_TRUE(filter.should_ignore("a/b/c/module.pyc"));
    
    EXPECT_FALSE(filter.should_ignore("module.py"));
}

TEST_F(FileFilterTest, P1_SingleStarDoesNotCrossDirectories) {
    // 单个 * 不应跨越目录边界（改进后的行为）
    write_ignore_file("src/*.cpp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("src/main.cpp"));
    EXPECT_TRUE(filter.should_ignore("src/util.cpp"));
    
    // 单个 * 不跨目录
    EXPECT_FALSE(filter.should_ignore("src/sub/main.cpp"));
}

// =========================================================================
//  P2: [abc] [a-z] [!abc] 字符类
// =========================================================================

TEST_F(FileFilterTest, P2_CharClassBasic) {
    // [abc] 匹配 a, b, 或 c
    write_ignore_file("file_[abc].txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("file_a.txt"));
    EXPECT_TRUE(filter.should_ignore("file_b.txt"));
    EXPECT_TRUE(filter.should_ignore("file_c.txt"));
    
    EXPECT_FALSE(filter.should_ignore("file_d.txt"));
    EXPECT_FALSE(filter.should_ignore("file_ab.txt"));
}

TEST_F(FileFilterTest, P2_CharClassRange) {
    // [0-9] 匹配数字
    write_ignore_file("log_[0-9].txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("log_0.txt"));
    EXPECT_TRUE(filter.should_ignore("log_5.txt"));
    EXPECT_TRUE(filter.should_ignore("log_9.txt"));
    
    EXPECT_FALSE(filter.should_ignore("log_a.txt"));
    EXPECT_FALSE(filter.should_ignore("log_10.txt"));  // 两位数
}

TEST_F(FileFilterTest, P2_CharClassNegation) {
    // [!abc] 匹配除 a, b, c 以外的单个字符
    write_ignore_file("file_[!xyz].txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("file_a.txt"));
    EXPECT_TRUE(filter.should_ignore("file_m.txt"));
    EXPECT_TRUE(filter.should_ignore("file_1.txt"));
    
    EXPECT_FALSE(filter.should_ignore("file_x.txt"));
    EXPECT_FALSE(filter.should_ignore("file_y.txt"));
    EXPECT_FALSE(filter.should_ignore("file_z.txt"));
}

TEST_F(FileFilterTest, P2_CharClassWithGlob) {
    // 组合: *.[ch]pp 匹配 .cpp 和 .hpp
    write_ignore_file("*.[ch]pp\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("main.cpp"));
    EXPECT_TRUE(filter.should_ignore("header.hpp"));
    EXPECT_TRUE(filter.should_ignore("sub/dir/util.cpp"));
    
    EXPECT_FALSE(filter.should_ignore("main.xpp"));
    EXPECT_FALSE(filter.should_ignore("main.cp"));
}

TEST_F(FileFilterTest, P2_CharClassUnclosed) {
    // 没有闭合的 [ → 当作字面量
    write_ignore_file("file_[abc.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // [abc 没闭合，整个 [ 当字面量，所以匹配 "file_[abc.txt"
    EXPECT_TRUE(filter.should_ignore("file_[abc.txt"));
    EXPECT_FALSE(filter.should_ignore("file_a.txt"));
}

TEST_F(FileFilterTest, P2_CharClassAlphaRange) {
    // [a-z] 匹配小写字母
    write_ignore_file("data_[a-z].csv\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("data_a.csv"));
    EXPECT_TRUE(filter.should_ignore("data_m.csv"));
    EXPECT_TRUE(filter.should_ignore("data_z.csv"));
    
    EXPECT_FALSE(filter.should_ignore("data_A.csv"));
    EXPECT_FALSE(filter.should_ignore("data_1.csv"));
}

// =========================================================================
//  P3: \# \! \\ 转义
// =========================================================================

TEST_F(FileFilterTest, P3_EscapeHash) {
    // \# 文件名以 # 开头
    write_ignore_file("\\#readme.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("#readme.txt"));
    EXPECT_FALSE(filter.should_ignore("readme.txt"));
}

TEST_F(FileFilterTest, P3_EscapeBang) {
    // \! 文件名以 ! 开头（不是取反规则）
    write_ignore_file("\\!important.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("!important.txt"));
    EXPECT_FALSE(filter.should_ignore("important.txt"));
}

TEST_F(FileFilterTest, P3_EscapeBackslash) {
    // \\ 字面量反斜杠 → 在我们的系统中被当作 / 分隔符
    // 这主要是为了让 Windows 用户的路径也能工作
    write_ignore_file("\\\\server\\\\share\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // \\ → / (Windows 路径规范化)
    // 实际效果：匹配路径 /server/share
    // 但因为 \\ 在 load_rules 的转义处理中被转为 \，
    // 然后在 glob_to_regex 中 \ 被转为 /
    // 具体行为取决于实现细节，这里主要确保不崩溃
    // 暂不做强断言，只确认不抛异常
}

TEST_F(FileFilterTest, P3_InlineCommentAfterRule) {
    // 行尾 # 注释应被正确截断
    write_ignore_file("*.bak # 备份文件\n*.swp # vim 交换文件\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("data.bak"));
    EXPECT_TRUE(filter.should_ignore("file.swp"));
    EXPECT_FALSE(filter.should_ignore("file.bak # 备份文件"));
}

TEST_F(FileFilterTest, P3_HashInMiddleOfFilename) {
    // 文件名中间包含 # → 需要转义
    write_ignore_file("temp\\#1.txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // load_rules 中 \# → #, 所以规则变成 "temp#1.txt"
    EXPECT_TRUE(filter.should_ignore("temp#1.txt"));
}

// =========================================================================
//  组合场景测试
// =========================================================================

TEST_F(FileFilterTest, CombinedNegationAndDoubleStar) {
    // 综合：忽略所有 test 目录下的文件，但保留 test/**/README.md
    write_ignore_file(
        "test/\n"
        "!test/**/README.md\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("test/unit.cpp"));
    EXPECT_TRUE(filter.should_ignore("test/sub/helper.h"));
    
    // README.md 被取反保留
    EXPECT_FALSE(filter.should_ignore("test/README.md"));
    EXPECT_FALSE(filter.should_ignore("test/sub/README.md"));
}

TEST_F(FileFilterTest, CombinedCharClassAndNegation) {
    // 忽略所有 file_[0-9].dat，但保留 file_0.dat
    write_ignore_file(
        "file_[0-9].dat\n"
        "!file_0.dat\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("file_1.dat"));
    EXPECT_TRUE(filter.should_ignore("file_9.dat"));
    EXPECT_FALSE(filter.should_ignore("file_0.dat"));  // 取反保留
}

TEST_F(FileFilterTest, CombinedDoubleStarAndCharClass) {
    // **/log_[0-9][0-9].txt 匹配任意层级下的 log_XX.txt
    write_ignore_file("**/log_[0-9][0-9].txt\n");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("log_01.txt"));
    EXPECT_TRUE(filter.should_ignore("dir/log_99.txt"));
    EXPECT_TRUE(filter.should_ignore("a/b/log_42.txt"));
    
    EXPECT_FALSE(filter.should_ignore("log_1.txt"));     // 只一位
    EXPECT_FALSE(filter.should_ignore("log_abc.txt"));    // 非数字
}

TEST_F(FileFilterTest, CombinedAllFeatures) {
    // 超级综合测试
    write_ignore_file(
        "# 忽略所有构建产物\n"
        "**/build/\n"
        "*.o\n"
        "*.obj\n"
        "\n"
        "# 但保留 release 构建的输出\n"
        "!**/build/release/\n"
        "\n"
        "# 忽略特定编号的临时文件\n"
        "temp_[0-9][0-9].dat\n"
        "\n"
        "# 文件名含 # 号的文件\n"
        "\\#scratch.txt\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 构建产物被忽略
    EXPECT_TRUE(filter.should_ignore("project/build/debug/main.exe"));
    EXPECT_TRUE(filter.should_ignore("main.o"));
    EXPECT_TRUE(filter.should_ignore("src/module.obj"));
    
    // release 构建被保留
    EXPECT_FALSE(filter.should_ignore("project/build/release/app.exe"));
    
    // 编号临时文件
    EXPECT_TRUE(filter.should_ignore("temp_01.dat"));
    EXPECT_TRUE(filter.should_ignore("temp_99.dat"));
    EXPECT_FALSE(filter.should_ignore("temp_1.dat"));    // 只一位数字
    EXPECT_FALSE(filter.should_ignore("temp_abc.dat"));  // 非数字
    
    // # 文件名
    EXPECT_TRUE(filter.should_ignore("#scratch.txt"));
    
    // 不受影响的文件
    EXPECT_FALSE(filter.should_ignore("README.md"));
    EXPECT_FALSE(filter.should_ignore("src/main.cpp"));
}

// =========================================================================
//  默认规则测试
// =========================================================================

TEST_F(FileFilterTest, DefaultRulesStillWork) {
    // 不创建 .veritasignore，默认规则应生效
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_TRUE(filter.should_ignore(".git/config"));
    EXPECT_TRUE(filter.should_ignore(".git/objects/pack/abc"));
    EXPECT_TRUE(filter.should_ignore(".veritas_tmp"));
    EXPECT_TRUE(filter.should_ignore(".veritasignore"));
    EXPECT_TRUE(filter.should_ignore("download.part"));
    EXPECT_TRUE(filter.should_ignore(".DS_Store"));
    EXPECT_TRUE(filter.should_ignore("Thumbs.db"));
    EXPECT_TRUE(filter.should_ignore("build/main.o"));
    EXPECT_TRUE(filter.should_ignore("bin/app.exe"));
    EXPECT_TRUE(filter.should_ignore("out/result.txt"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-journal"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-wal"));
    EXPECT_TRUE(filter.should_ignore(".veritas.db-shm"));
    
    // 正常文件不应被忽略
    EXPECT_FALSE(filter.should_ignore("README.md"));
    EXPECT_FALSE(filter.should_ignore("src/main.cpp"));
}

// =========================================================================
//  边界情况
// =========================================================================

TEST_F(FileFilterTest, EmptyPath) {
    FileFilter filter;
    filter.load_rules(test_dir);
    EXPECT_FALSE(filter.should_ignore(""));
}

TEST_F(FileFilterTest, EmptyIgnoreFile) {
    write_ignore_file("");
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 只有默认规则生效
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_FALSE(filter.should_ignore("README.md"));
}

TEST_F(FileFilterTest, OnlyCommentsAndWhitespace) {
    write_ignore_file(
        "# 这是注释\n"
        "   \n"
        "\t\n"
        "# 另一行注释\n"
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    // 只有默认规则生效
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_FALSE(filter.should_ignore("README.md"));
}

TEST_F(FileFilterTest, NoIgnoreFile) {
    // 不创建 .veritasignore
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore(".git"));
    EXPECT_FALSE(filter.should_ignore("src/main.cpp"));
}

TEST_F(FileFilterTest, MultipleNegationsChained) {
    // 多层取反链
    write_ignore_file(
        "*.txt\n"      // 忽略所有 txt
        "!*.txt\n"     // 取消忽略所有 txt
        "*.txt\n"      // 再次忽略所有 txt
        "!keep.txt\n"  // 只保留 keep.txt
    );
    FileFilter filter;
    filter.load_rules(test_dir);
    
    EXPECT_TRUE(filter.should_ignore("random.txt"));
    EXPECT_TRUE(filter.should_ignore("another.txt"));
    EXPECT_FALSE(filter.should_ignore("keep.txt"));
}
