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

// ---------------------------------------------------------
// 3. 错误诊断辅助函数
// ---------------------------------------------------------

// 获取当前系统错误信息（包含 errno 和 Win32 错误码）
// 用于文件操作失败时的诊断日志
inline std::string GetLastSystemError() {
#ifdef _WIN32
    DWORD win_err = GetLastError();
    int posix_err = errno;
    
    std::string result = "errno=" + std::to_string(posix_err);
    if (posix_err != 0) {
        result += " (";
        switch (posix_err) {
            case ENOENT: result += "ENOENT:文件不存在"; break;
            case EACCES: result += "EACCES:权限拒绝"; break;
            case EMFILE: result += "EMFILE:进程句柄超限"; break;
            case ENFILE: result += "ENFILE:系统句柄超限"; break;
            case ENOSPC: result += "ENOSPC:磁盘空间不足"; break;
            case EEXIST: result += "EEXIST:文件已存在"; break;
            case ENOTEMPTY: result += "ENOTEMPTY:目录非空"; break;
            case EBUSY: result += "EBUSY:资源忙"; break;
            default: result += "未知POSIX错误"; break;
        }
        result += ")";
    }
    
    if (win_err != 0) {
        result += ", Win32=" + std::to_string(win_err);
        wchar_t buf[256] = {0};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, win_err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buf, 255, NULL);
        std::wstring wmsg(buf);
        // 去除末尾换行
        while (!wmsg.empty() && (wmsg.back() == L'\r' || wmsg.back() == L'\n')) {
            wmsg.pop_back();
        }
        if (!wmsg.empty()) {
            result += " (" + WideToUtf8(wmsg) + ")";
        }
    }
    return result;
#else
    int posix_err = errno;
    std::string result = "errno=" + std::to_string(posix_err);
    if (posix_err != 0) {
        result += " (" + std::string(strerror(posix_err)) + ")";
    }
    return result;
#endif
}

// 格式化 std::error_code 为可读字符串
inline std::string FormatErrorCode(const std::error_code& ec) {
    if (!ec) return "OK";
    return "code=" + std::to_string(ec.value()) + " (" + ec.message() + ")";
}

}  // namespace VeritasSync