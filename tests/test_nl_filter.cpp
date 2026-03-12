// tests/test_nl_filter.cpp
// 自然语言过滤规则生成器单元测试

#include "test_helpers.h"

#include <gtest/gtest.h>
#include <string>
#include <algorithm>

#include "VeritasSync/storage/NLFilterGenerator.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

class NLFilterGeneratorTest : public ::testing::Test {
protected:
    NLFilterGenerator generator;
};

// ═══════════════════════════════════════════════════════════════
// 基础模板匹配测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, IgnoreLogFiles) {
    auto result = generator.generate("忽略所有日志文件");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.source, "template");
    EXPECT_NE(result.rules.find("*.log"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreLogFilesEnglish) {
    auto result = generator.generate("忽略 log 文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.log"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreTempFiles) {
    auto result = generator.generate("忽略临时文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.tmp"), std::string::npos);
    EXPECT_NE(result.rules.find("*.bak"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreBuildDir) {
    auto result = generator.generate("忽略编译产物");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("build/"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreImages) {
    auto result = generator.generate("忽略所有图片文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.jpg"), std::string::npos);
    EXPECT_NE(result.rules.find("*.png"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreVideoFiles) {
    auto result = generator.generate("忽略视频文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.mp4"), std::string::npos);
    EXPECT_NE(result.rules.find("*.avi"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreAudioFiles) {
    auto result = generator.generate("忽略音频文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.mp3"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreArchives) {
    auto result = generator.generate("忽略压缩文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.zip"), std::string::npos);
    EXPECT_NE(result.rules.find("*.rar"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreNodeModules) {
    auto result = generator.generate("忽略 node_modules");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("node_modules/"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnorePythonCache) {
    auto result = generator.generate("忽略 python 缓存和 pycache");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("__pycache__/"), std::string::npos);
    EXPECT_NE(result.rules.find("*.pyc"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreIDEConfig) {
    auto result = generator.generate("忽略 IDE 配置文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".idea/"), std::string::npos);
    EXPECT_NE(result.rules.find(".vscode/"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreEnvFiles) {
    auto result = generator.generate("忽略环境变量文件和密钥");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".env"), std::string::npos);
    EXPECT_NE(result.rules.find("*.pem"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreDatabase) {
    auto result = generator.generate("忽略数据库文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.db"), std::string::npos);
    EXPECT_NE(result.rules.find("*.sqlite"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreDocuments) {
    auto result = generator.generate("忽略办公文档");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.docx"), std::string::npos);
    EXPECT_NE(result.rules.find("*.xlsx"), std::string::npos);
    EXPECT_NE(result.rules.find("*.pdf"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreCache) {
    auto result = generator.generate("忽略缓存目录");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".cache/"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreHiddenFiles) {
    auto result = generator.generate("忽略所有隐藏文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".*"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 多模板组合匹配测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, CombineLogAndTemp) {
    auto result = generator.generate("忽略所有日志和临时文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.log"), std::string::npos);
    EXPECT_NE(result.rules.find("*.tmp"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, CombineImagesAndVideos) {
    auto result = generator.generate("忽略图片和视频");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.jpg"), std::string::npos);
    EXPECT_NE(result.rules.find("*.mp4"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, CombineBuildAndCache) {
    auto result = generator.generate("忽略构建产物和缓存");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("build/"), std::string::npos);
    EXPECT_NE(result.rules.find(".cache/"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 取反（白名单）测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, ExceptionKeep) {
    auto result = generator.generate("忽略所有日志文件，但保留 error.log");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.log"), std::string::npos);
    EXPECT_NE(result.rules.find("!error.log"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, ExceptionExcept) {
    auto result = generator.generate("忽略临时文件，除了 important.tmp");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.tmp"), std::string::npos);
    EXPECT_NE(result.rules.find("!important.tmp"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 大小写不敏感测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, CaseInsensitiveKeyword) {
    auto result = generator.generate("忽略 BUILD 目录");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("build/"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, CaseInsensitiveLog) {
    auto result = generator.generate("忽略 LOG 文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.log"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 模板无法匹配的情况
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, UnrecognizedDescription) {
    // 没有 LLM 配置时，无法识别的描述应返回失败
    auto result = generator.generate("请帮我做一些完全不相关的事情");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(NLFilterGeneratorTest, EmptyDescription) {
    auto result = generator.generate("");
    EXPECT_FALSE(result.success);
}

// ═══════════════════════════════════════════════════════════════
// LLM 配置测试（不实际调用网络）
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, NoLLMByDefault) {
    EXPECT_FALSE(generator.has_llm());
}

TEST_F(NLFilterGeneratorTest, SetLLMConfig) {
    NLFilterGenerator::LLMConfig config;
    config.api_url = "https://api.example.com";
    config.api_key = "test-key";
    config.model = "test-model";
    generator.set_llm_config(config);
    EXPECT_TRUE(generator.has_llm());
}

TEST_F(NLFilterGeneratorTest, EmptyLLMConfig) {
    NLFilterGenerator::LLMConfig config;
    config.api_url = "";
    config.api_key = "";
    generator.set_llm_config(config);
    EXPECT_FALSE(generator.has_llm());
}

TEST_F(NLFilterGeneratorTest, PartialLLMConfig) {
    NLFilterGenerator::LLMConfig config;
    config.api_url = "https://api.example.com";
    config.api_key = "";  // 缺少 key
    generator.set_llm_config(config);
    EXPECT_FALSE(generator.has_llm());
}

// ═══════════════════════════════════════════════════════════════
// generate_from_template 直接调用测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, DirectTemplateCall) {
    auto result = generator.generate_from_template("忽略日志");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.source, "template");
}

TEST_F(NLFilterGeneratorTest, TemplateFailsGracefully) {
    auto result = generator.generate_from_template("xyzzy");
    EXPECT_FALSE(result.success);
}

// ═══════════════════════════════════════════════════════════════
// generate_from_llm 无配置时的行为
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, LLMWithoutConfig) {
    auto result = generator.generate_from_llm("忽略所有日志");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("未配置"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 结果结构体测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, ResultStructDefaults) {
    NLFilterGenerator::Result r;
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.rules.empty());
    EXPECT_TRUE(r.explanation.empty());
    EXPECT_TRUE(r.source.empty());
    EXPECT_TRUE(r.error.empty());
}

// ═══════════════════════════════════════════════════════════════
// 各种常见语言的自然语言表述测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, VariousPhrasings_Log) {
    // "日志" 是关键词
    EXPECT_TRUE(generator.generate("不要同步日志").success);
    EXPECT_TRUE(generator.generate("过滤掉所有日志文件").success);
    EXPECT_TRUE(generator.generate("跳过log").success);
    EXPECT_TRUE(generator.generate("排除.log文件").success);
}

TEST_F(NLFilterGeneratorTest, VariousPhrasings_Build) {
    EXPECT_TRUE(generator.generate("忽略 build 目录").success);
    EXPECT_TRUE(generator.generate("不同步编译输出").success);
    EXPECT_TRUE(generator.generate("跳过构建产物").success);
}

TEST_F(NLFilterGeneratorTest, VariousPhrasings_NodeModules) {
    EXPECT_TRUE(generator.generate("忽略 node_modules 目录").success);
    EXPECT_TRUE(generator.generate("不同步 npm 依赖").success);
    EXPECT_TRUE(generator.generate("过滤前端依赖").success);
}

// ═══════════════════════════════════════════════════════════════
// Java/Gradle 相关测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, IgnoreJavaBuild) {
    auto result = generator.generate("忽略 Java 编译产物和 maven");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.class"), std::string::npos);
    EXPECT_NE(result.rules.find("target/"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// macOS / Windows 系统文件测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, IgnoreMacOS) {
    auto result = generator.generate("忽略 macOS 系统文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".DS_Store"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, IgnoreWindows) {
    auto result = generator.generate("忽略 Windows 系统文件 Thumbs.db");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("Thumbs.db"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// 大文件 / 二进制文件测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, IgnoreBinaryFiles) {
    auto result = generator.generate("忽略二进制文件和大文件");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find("*.iso"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// Git 目录测试
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, IgnoreGitDir) {
    auto result = generator.generate("忽略 .git 目录");
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.rules.find(".git/"), std::string::npos);
}
