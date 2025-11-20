#pragma once

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace VeritasSync {

// ---------------------------------------------------------
// 1. 路径转换工具
// ---------------------------------------------------------

// 将 UTF-8 std::string 转换为 std::filesystem::path
// 核心解决：Windows 下 path 默认构造函数视 string 为 ANSI (GBK)，必须转为 u8string 或 wstring
inline std::filesystem::path Utf8ToPath(const std::string& utf8_str) {
#ifdef _WIN32
    // C++20 标准写法：通过 char8_t 显式告知这是 UTF-8
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8_str.c_str())));
#else
    // Linux/macOS 原生支持 UTF-8
    return std::filesystem::path(utf8_str);
#endif
}

// 将 std::filesystem::path 转换为 UTF-8 std::string
// 核心解决：path.string() 在 Windows 上可能返回 GBK，导致乱码
inline std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    std::u8string u8_str = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8_str.c_str()));
#else
    return path.string();
#endif
}

// ---------------------------------------------------------
// 2. Windows API 边界转换工具 (仅 Windows 有效)
// ---------------------------------------------------------
#ifdef _WIN32

// 将 UTF-8 string 转为 Windows API 需要的 UTF-16 wstring
inline std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// 将 Windows API 返回的 wstring 转回 UTF-8 string
inline std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

#endif

}  // namespace VeritasSync