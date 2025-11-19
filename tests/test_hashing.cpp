#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "VeritasSync/Hashing.h"

using namespace VeritasSync;

class HashingTest : public ::testing::Test {
protected:
    std::string temp_file = "test_hash.tmp";

    void SetUp() override {
        // 每个测试前创建一个已知内容的文件
        std::ofstream f(temp_file);
        f << "hello world";  // sha256: b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
        f.close();
    }

    void TearDown() override { std::filesystem::remove(temp_file); }
};

TEST_F(HashingTest, CalculateCorrectSHA256) {
    std::string hash = Hashing::CalculateSHA256(temp_file);
    // "hello world" 的 SHA256
    EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(HashingTest, HandleMissingFile) {
    std::string hash = Hashing::CalculateSHA256("non_existent_file.xyz");
    EXPECT_EQ(hash, "");  // 假设找不到文件返回空字符串
}