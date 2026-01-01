// tests/test_common_utils_robustness.cpp
#include <gtest/gtest.h>
#include "VeritasSync/common/ModernUtils.h"
#include "VeritasSync/common/EncodingUtils.h"
#include <filesystem>

using namespace VeritasSync;

// 1. 测试 ModernUtils Result 和 Error
TEST(CommonUtilsRobustness, ResultAndErrorMapping) {
    // 测试成功路径
    Result<int> res(42);
    EXPECT_TRUE(res.has_value());
    EXPECT_EQ(res.value(), 42);
    EXPECT_EQ(*res, 42);

    // 测试映射功能 (Chain transformations)
    auto res_str = res.map([](int v) { return std::to_string(v) + "_wrapped"; });
    EXPECT_TRUE(res_str.has_value());
    EXPECT_EQ(res_str.value(), "42_wrapped");

    // 测试错误路径
    Result<int> err_res(Error::io(5, "disk full", "C:/data"));
    EXPECT_TRUE(err_res.has_error());
    EXPECT_EQ(err_res.error().category, ErrorCategory::IO);
    EXPECT_EQ(err_res.error().code, 5);
    EXPECT_EQ(err_res.error().context, "C:/data");

    // 错误状态的映射应保持错误
    auto err_mapped = err_res.map([](int v) { return v * 2; });
    EXPECT_TRUE(err_mapped.has_error());
    EXPECT_EQ(err_mapped.error().message, "disk full");
}

// 2. 测试 EncodingUtils 的鲁棒性 (跨平台路径处理)
TEST(CommonUtilsRobustness, PathEncodingRobustness) {
    // 测试包含空格、中文字符、特殊符号的路径
    // Windows 下这些最容易出乱码
    std::string complex_path = "Sync/测试 目录/file with spaces & symbols !@#.txt";
    
    // 模拟 Utf8 -> Path -> Utf8 的完整循环
    auto path_obj = Utf8ToPath(complex_path);
    std::string back_to_utf8 = PathToUtf8(path_obj);
    
    // 在所有平台上，内容应当保持一致
    EXPECT_EQ(complex_path, back_to_utf8);
    
    // 测试空路径
    EXPECT_TRUE(PathToUtf8(Utf8ToPath("")).empty());
}

// 3. 测试 SystemError 解析
TEST(CommonUtilsRobustness, SystemErrorFormatting) {
    std::string err_msg = GetLastSystemError();
    // 即使没有错误（errno=0），也不应该崩溃，通常返回 "errno=0"
    EXPECT_FALSE(err_msg.empty());
    
    std::error_code ec = std::make_error_code(std::errc::no_such_file_or_directory);
    std::string formatted = FormatErrorCode(ec);
    EXPECT_TRUE(formatted.find("code=") != std::string::npos);
}

// 4. 测试 Span 工具
TEST(CommonUtilsRobustness, SpanSafety) {
    std::string data = "VeritasSync";
    auto s = make_span(data);
    EXPECT_EQ(s.size(), data.size());
    EXPECT_EQ(s[0], 'V');
    
    auto back = to_string(s);
    EXPECT_EQ(back, data);
}
