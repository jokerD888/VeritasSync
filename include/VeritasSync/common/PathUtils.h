#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
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
            if (a.length() != b.length()) {
                return false;
            }
            return std::equal(a.begin(), a.end(), b.begin(),
                             [](char ca, char cb) {
                                 return std::tolower(static_cast<unsigned char>(ca)) ==
                                        std::tolower(static_cast<unsigned char>(cb));
                             });
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
     * @brief 验证相对路径是否安全（不逃逸出根目录）
     * 
     * 防止路径遍历攻击：检查 relative_path 与 root 拼接后
     * 是否仍在 root 目录下。
     * 
     * 检查规则：
     * 1. 拒绝包含 ".." 路径组件的路径（直接在字符串层面检查）
     * 2. 拒绝绝对路径（防止覆盖 root）
     * 3. 归一化后确认最终路径以 root 为前缀
     * 
     * @param root 同步根目录（必须是已存在的绝对路径）
     * @param relative_path 来自网络的相对路径字符串
     * @return true 路径安全，false 存在遍历风险
     */
    static bool is_path_safe(const std::filesystem::path& root, const std::string& relative_path) {
        std::error_code ec;
        auto canonical_root = std::filesystem::weakly_canonical(root, ec);
        if (ec) return false;
        return is_path_safe_with_canonical_root(canonical_root, relative_path);
    }

    /**
     * @brief 批量路径安全检查优化版：root 已 canonicalize，避免重复 I/O
     */
    static bool is_path_safe_with_canonical_root(const std::filesystem::path& canonical_root,
                                                  const std::string& relative_path) {
        // 空路径不安全
        if (relative_path.empty()) return false;

        // 检查是否包含 ".." 组件（逐组件检查，避免被 "a..b" 误判）
        std::filesystem::path rel(relative_path);
        for (const auto& component : rel) {
            if (component == "..") return false;
        }

        // 检查是否是绝对路径（绝对路径会覆盖 root）
        if (rel.is_absolute()) return false;

        // 最终确认：拼接后归一化，确保在 root 下
        std::error_code ec;
        auto full = std::filesystem::weakly_canonical(canonical_root / rel, ec);
        if (ec) return false;

        // 检查 full 路径是否以 root 为前缀
        auto root_str = canonical_root.string();
        auto full_str = full.string();

        #ifdef _WIN32
        // Windows 路径不区分大小写
        std::transform(root_str.begin(), root_str.end(), root_str.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::transform(full_str.begin(), full_str.end(), full_str.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        #endif

        // full_str 必须以 root_str 为前缀，且紧接着是分隔符或结尾
        if (full_str.length() < root_str.length()) return false;
        if (full_str.compare(0, root_str.length(), root_str) != 0) return false;
        if (full_str.length() > root_str.length()) {
            char next_char = full_str[root_str.length()];
            if (next_char != '/' && next_char != '\\') return false;
        }

        return true;
    }

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
