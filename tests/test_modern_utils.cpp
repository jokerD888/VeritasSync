// tests/test_modern_utils.cpp
// 测试 C++20 现代化工具

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "VeritasSync/common/ModernUtils.h"

using namespace VeritasSync;

class ModernUtilsTest : public ::testing::Test {};

// =====================================================
// std::span 辅助函数测试
// =====================================================

TEST_F(ModernUtilsTest, MakeSpan_FromPointer) {
    const char* data = "hello";
    auto span = make_span(data, 5);
    
    EXPECT_EQ(span.size(), 5);
    EXPECT_EQ(span[0], 'h');
    EXPECT_EQ(span[4], 'o');
}

TEST_F(ModernUtilsTest, MakeSpan_FromString) {
    std::string str = "world";
    auto span = make_span(str);
    
    EXPECT_EQ(span.size(), 5);
    EXPECT_EQ(span[0], 'w');
}

TEST_F(ModernUtilsTest, ToString_FromSpan) {
    std::vector<char> data{'t', 'e', 's', 't'};
    auto span = std::span<const char>{data.data(), data.size()};
    
    std::string result = to_string(span);
    EXPECT_EQ(result, "test");
}

TEST_F(ModernUtilsTest, AsBytes_FromString) {
    std::string str = "abc";
    auto bytes = as_bytes(str);
    
    EXPECT_EQ(bytes.size(), 3);
    EXPECT_EQ(std::to_integer<char>(bytes[0]), 'a');
}

// =====================================================
// Error 类型测试
// =====================================================

TEST_F(ModernUtilsTest, Error_None) {
    Error err = Error::none();
    
    EXPECT_FALSE(err);
    EXPECT_TRUE(err.ok());
    EXPECT_FALSE(err.is_error());
    EXPECT_EQ(err.category, ErrorCategory::None);
}

TEST_F(ModernUtilsTest, Error_IO) {
    Error err = Error::io(5, "File not found", "/path/to/file");
    
    EXPECT_TRUE(err);
    EXPECT_FALSE(err.ok());
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.category, ErrorCategory::IO);
    EXPECT_EQ(err.code, 5);
    EXPECT_EQ(err.message, "File not found");
    EXPECT_EQ(err.context, "/path/to/file");
}

TEST_F(ModernUtilsTest, Error_FullMessage) {
    Error err = Error::network(100, "Connection failed", "peer_123");
    
    std::string msg = err.full_message();
    EXPECT_TRUE(msg.find("Connection failed") != std::string::npos);
    EXPECT_TRUE(msg.find("100") != std::string::npos);
    EXPECT_TRUE(msg.find("peer_123") != std::string::npos);
}

TEST_F(ModernUtilsTest, Error_NoMessage_Empty) {
    Error err = Error::none();
    EXPECT_EQ(err.full_message(), "");
}

TEST_F(ModernUtilsTest, Error_Crypto) {
    Error err = Error::crypto("Key derivation failed");
    
    EXPECT_EQ(err.category, ErrorCategory::Crypto);
    EXPECT_EQ(err.message, "Key derivation failed");
}

TEST_F(ModernUtilsTest, Error_Timeout) {
    Error err = Error::timeout("Operation timed out after 30s");
    
    EXPECT_EQ(err.category, ErrorCategory::Timeout);
}

TEST_F(ModernUtilsTest, Error_Cancelled) {
    Error err = Error::cancelled();
    
    EXPECT_EQ(err.category, ErrorCategory::Cancelled);
    EXPECT_TRUE(err.is_error());
}

// =====================================================
// Result<T> 类型测试
// =====================================================

TEST_F(ModernUtilsTest, Result_Success) {
    Result<int> result(42);
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(result.has_error());
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(*result, 42);
}

TEST_F(ModernUtilsTest, Result_Error) {
    Result<int> result(Error::io(1, "Read failed"));
    
    EXPECT_FALSE(result);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.has_error());
    EXPECT_EQ(result.error().category, ErrorCategory::IO);
}

TEST_F(ModernUtilsTest, Result_ValueOr) {
    Result<int> success(42);
    Result<int> failure(Error::internal("fail"));
    
    EXPECT_EQ(success.value_or(0), 42);
    EXPECT_EQ(failure.value_or(-1), -1);
}

TEST_F(ModernUtilsTest, Result_String) {
    StringResult result(std::string("hello world"));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(*result, "hello world");
}

TEST_F(ModernUtilsTest, Result_Map) {
    Result<int> result(10);
    
    auto mapped = result.map([](int x) { return x * 2; });
    
    EXPECT_TRUE(mapped);
    EXPECT_EQ(*mapped, 20);
}

TEST_F(ModernUtilsTest, Result_Map_Error) {
    Result<int> result(Error::internal("fail"));
    
    auto mapped = result.map([](int x) { return x * 2; });
    
    EXPECT_FALSE(mapped);
    EXPECT_TRUE(mapped.has_error());
}

TEST_F(ModernUtilsTest, Result_Void_Success) {
    VoidResult result;
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(result.has_value());
}

TEST_F(ModernUtilsTest, Result_Void_Error) {
    VoidResult result(Error::network(0, "Connection lost"));
    
    EXPECT_FALSE(result);
    EXPECT_TRUE(result.has_error());
    EXPECT_EQ(result.error().category, ErrorCategory::Network);
}

TEST_F(ModernUtilsTest, Result_FromOptional_HasValue) {
    std::optional<std::string> opt = "test";
    auto result = Result<std::string>::from_optional(opt, Error::internal("no value"));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(*result, "test");
}

TEST_F(ModernUtilsTest, Result_FromOptional_Empty) {
    std::optional<std::string> opt = std::nullopt;
    auto result = Result<std::string>::from_optional(opt, Error::internal("no value"));
    
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().message, "no value");
}

TEST_F(ModernUtilsTest, Result_Pointer_Access) {
    Result<std::string> result(std::string("hello"));
    
    EXPECT_EQ(result->length(), 5);
}

// =====================================================
// 类型别名测试
// =====================================================

TEST_F(ModernUtilsTest, TypeAliases_CharSpan) {
    std::string data = "test";
    CharSpan span{data.data(), data.size()};
    
    EXPECT_EQ(span.size(), 4);
}

TEST_F(ModernUtilsTest, BoolResult_Success) {
    BoolResult result(true);
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(*result);
}

TEST_F(ModernUtilsTest, BoolResult_Failure) {
    BoolResult result(Error::protocol("Invalid message"));
    
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().category, ErrorCategory::Protocol);
}
