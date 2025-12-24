// tests/test_encoding_utils.cpp
// 测试路径编码工具的正确性

#include <gtest/gtest.h>
#include <string>
#include <filesystem>

#include "VeritasSync/common/EncodingUtils.h"

using namespace VeritasSync;

class EncodingUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// --- Utf8ToPath 测试 ---

TEST_F(EncodingUtilsTest, Utf8ToPath_AsciiOnly) {
    std::string utf8_path = "folder/subfolder/file.txt";
    std::filesystem::path result = Utf8ToPath(utf8_path);
    
    // 验证路径字符串能正确还原
    std::string back = PathToUtf8(result);
    EXPECT_EQ(back, utf8_path);
}

TEST_F(EncodingUtilsTest, Utf8ToPath_EmptyPath) {
    std::string utf8_path = "";
    std::filesystem::path result = Utf8ToPath(utf8_path);
    
    EXPECT_TRUE(result.empty());
}

TEST_F(EncodingUtilsTest, Utf8ToPath_SpecialCharacters) {
    std::string utf8_path = "folder with spaces/file-name_123.txt";
    std::filesystem::path result = Utf8ToPath(utf8_path);
    
    std::string back = PathToUtf8(result);
    EXPECT_EQ(back, utf8_path);
}

// --- PathToUtf8 测试 ---

TEST_F(EncodingUtilsTest, PathToUtf8_FromFilesystemPath) {
    std::filesystem::path p = std::filesystem::current_path();
    std::string utf8 = PathToUtf8(p);
    
    // 应该能成功转换，不应该为空
    EXPECT_FALSE(utf8.empty());
}

// --- Utf8ToWide 和 WideToUtf8 测试 ---

#ifdef _WIN32

TEST_F(EncodingUtilsTest, Utf8ToWide_AsciiOnly) {
    std::string utf8 = "Hello World";
    std::wstring wide = Utf8ToWide(utf8);
    
    EXPECT_EQ(wide, L"Hello World");
}

TEST_F(EncodingUtilsTest, WideToUtf8_Empty) {
    std::wstring empty;
    std::string result = WideToUtf8(empty);
    EXPECT_TRUE(result.empty());
}

TEST_F(EncodingUtilsTest, Utf8ToWide_Empty) {
    std::string empty;
    std::wstring result = Utf8ToWide(empty);
    EXPECT_TRUE(result.empty());
}

TEST_F(EncodingUtilsTest, Utf8WideRoundtrip) {
    std::string original = "Hello World 123";
    std::wstring wide = Utf8ToWide(original);
    std::string back = WideToUtf8(wide);
    EXPECT_EQ(back, original);
}

#endif

// --- 往返测试 (Roundtrip) ---

TEST_F(EncodingUtilsTest, PathRoundtrip_Simple) {
    std::string original = "Project/source/README.md";
    
    std::filesystem::path p = Utf8ToPath(original);
    std::string back = PathToUtf8(p);
    
    EXPECT_EQ(back, original);
}

// --- 边界情况测试 ---

TEST_F(EncodingUtilsTest, PathWithDotsAndSpaces) {
    std::string path = "folder . name/ file .. name .txt";
    
    std::filesystem::path p = Utf8ToPath(path);
    std::string back = PathToUtf8(p);
    
    EXPECT_EQ(back, path);
}

TEST_F(EncodingUtilsTest, DeepNestedPath) {
    std::string deep_path;
    for (int i = 0; i < 10; ++i) {
        deep_path += "level" + std::to_string(i) + "/";
    }
    deep_path += "file.txt";
    
    std::filesystem::path p = Utf8ToPath(deep_path);
    std::string back = PathToUtf8(p);
    
    EXPECT_EQ(back, deep_path);
}
