#pragma once

#include <filesystem>
#include <regex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace VeritasSync {

// C++20 异构查找支持：允许 std::unordered_set 使用 string_view 进行查找而无需创建临时 string
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
    size_t operator()(const std::string& s) const { return std::hash<std::string>{}(s); }
};

class FileFilter {
public:
    FileFilter();

    void load_rules(const std::filesystem::path& root_path);

    // 检查路径是否应该被忽略 (使用 string_view 实现零拷贝)
    // 注意：输入路径应已规范化为使用 '/' 作为分隔符
    bool should_ignore(std::string_view path) const;

private:
    // 编译后的单条规则
    struct CompiledRule {
        std::regex regex;          // 匹配用的正则
        bool negated = false;      // 是否为 ! 取反规则
        std::string original;      // 原始规则文本（调试用）
    };

    // 快速路径：后缀和目录的匹配缓存（仅用于非取反的默认规则）
    struct FastPath {
        std::unordered_set<std::string, StringHash, std::equal_to<>> exts;   // ".ext"
        std::unordered_set<std::string, StringHash, std::equal_to<>> dirs;   // "dirname"
    };

    void parse_rule(const std::string& raw_rule, bool is_default = false, bool bang_escaped = false);
    static std::string glob_to_regex(const std::string& glob);

    // 默认规则的快速路径（不可被取反覆盖）
    FastPath m_default_fast;
    // 默认规则中需要正则的（不可被取反覆盖）
    std::vector<std::regex> m_default_complex;

    // 用户规则列表（按声明顺序，支持取反）
    std::vector<CompiledRule> m_user_rules;

    // 用户非取反规则的快速路径缓存（加速常见匹配）
    FastPath m_user_fast;
    // 标记：用户规则中是否包含任何取反规则
    bool m_has_negation = false;

    mutable std::shared_mutex m_mutex;
};

}  // namespace VeritasSync
