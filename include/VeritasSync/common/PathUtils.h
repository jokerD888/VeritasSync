#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <functional>

namespace VeritasSync {

/**
 * @brief 路径处理工具类
 * 
 * 提供跨平台路径归一化、大小写不敏感比较和哈希功能。
 * 
 * 设计原则：
 * - Windows: 路径不区分大小写 (C:\Temp == c:\temp)
 * - Linux/macOS: 路径区分大小写 (但我们统一处理以避免混乱)
 */
class PathUtils {
public:
    /**
     * @brief 归一化路径
     * 
     * 操作：
     * 1. 统一路径分隔符为 '/'
     * 2. Windows下转小写（可选，取决于策略）
     * 3. 移除尾部斜杠
     * 
     * @param path 原始路径
     * @return 归一化后的路径
     */
    static std::string normalize(std::string path) {
        if (path.empty()) {
            return path;
        }

        // 1. 统一路径分隔符
        std::replace(path.begin(), path.end(), '\\', '/');

        // 2. Windows 下转小写（确保不区分大小写）
        #ifdef _WIN32
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        #endif

        // 3. 移除尾部斜杠（除非是根目录）
        while (path.length() > 1 && path.back() == '/') {
            path.pop_back();
        }

        return path;
    }

    /**
     * @brief 大小写不敏感的路径比较
     * 
     * 用作 unordered_map 的比较器
     */
    struct CaseInsensitiveEqual {
        bool operator()(const std::string& a, const std::string& b) const {
            #ifdef _WIN32
            // Windows: 不区分大小写
            if (a.length() != b.length()) {
                return false;
            }
            return std::equal(a.begin(), a.end(), b.begin(),
                             [](char ca, char cb) {
                                 return std::tolower(static_cast<unsigned char>(ca)) ==
                                        std::tolower(static_cast<unsigned char>(cb));
                             });
            #else
            // Linux/macOS: 区分大小写（但为了一致性，也可以不区分）
            // 这里选择在所有平台统一不区分
            if (a.length() != b.length()) {
                return false;
            }
            return std::equal(a.begin(), a.end(), b.begin(),
                             [](char ca, char cb) {
                                 return std::tolower(static_cast<unsigned char>(ca)) ==
                                        std::tolower(static_cast<unsigned char>(cb));
                             });
            #endif
        }
    };

    /**
     * @brief 大小写不敏感的路径哈希
     * 
     * 用作 unordered_map 的哈希函数
     */
    struct CaseInsensitiveHash {
        size_t operator()(const std::string& str) const {
            std::string lower = str;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            return std::hash<std::string>{}(lower);
        }
    };

    /**
     * @brief 检查两个路径是否相等（不区分大小写）
     */
    static bool are_equal(const std::string& a, const std::string& b) {
        return CaseInsensitiveEqual{}(a, b);
    }

    /**
     * @brief 计算路径的哈希值（不区分大小写）
     */
    static size_t hash(const std::string& path) {
        return CaseInsensitiveHash{}(path);
    }
};

}  // namespace VeritasSync
