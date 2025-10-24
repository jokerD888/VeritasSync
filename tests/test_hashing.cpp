#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "VeritasSync/Hashing.h"

// 测试 Hashing 工具类
TEST(HashingTest, CalculateSHA256) {
  // 1. 准备一个临时文件
  const std::string test_filename = "temp_hash_test.txt";
  const std::string test_content = "VeritasSync SHA256 Test";

  // 写入文件
  {
    std::ofstream ofs(test_filename);
    ofs << test_content;
  }  // 确保文件在继续之前关闭

  // 2. 这是 "VeritasSync SHA256 Test" 字符串的已知 SHA256 哈希值
  //    (已从 f160... 修正为 69ccd...)
  const std::string expected_hash =
      "69ccd72c90e0d9c165ddc77aac72a6382841b6380fe17683d493c351d0764942";

  // 3. 执行测试
  std::string calculated_hash =
      VeritasSync::Hashing::CalculateSHA256(test_filename);

  // 4. 断言
  ASSERT_EQ(calculated_hash, expected_hash);

  // 5. 清理
  std::filesystem::remove(test_filename);
}

TEST(HashingTest, FileNotExist) {
  // 1. 准备一个不存在的文件
  const std::string test_filename = "a_file_that_does_not_exist.txt";

  // 2. 确保它不存在 (以防万一)
  std::filesystem::remove(test_filename);

  // 3. 执行测试
  std::string calculated_hash =
      VeritasSync::Hashing::CalculateSHA256(test_filename);

  // 4. 断言 (失败时应返回空字符串)
  ASSERT_TRUE(calculated_hash.empty());
}