// tests/test_nl_filter.cpp
// 自然语言过滤规则生成器单元测试

#include "test_helpers.h"

#include <gtest/gtest.h>
#include <string>

#include "VeritasSync/storage/NLFilterGenerator.h"

using namespace VeritasSync;

REGISTER_VERITAS_TEST_ENV();

class NLFilterGeneratorTest : public ::testing::Test {
protected:
    NLFilterGenerator generator;
};

// ═══════════════════════════════════════════════════════════════
// LLM 配置测试
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
// 无 LLM 配置时的行为
// ═══════════════════════════════════════════════════════════════

TEST_F(NLFilterGeneratorTest, GenerateWithoutLLM) {
    auto result = generator.generate("忽略所有日志文件");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("API Key"), std::string::npos);
}

TEST_F(NLFilterGeneratorTest, EmptyDescription) {
    auto result = generator.generate("");
    EXPECT_FALSE(result.success);
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
