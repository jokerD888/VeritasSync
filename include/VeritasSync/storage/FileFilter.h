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
    void classify_rule(const std::string& rule);

    std::unordered_set<std::string, StringHash, std::equal_to<>> m_ignored_exts; 
    std::unordered_set<std::string, StringHash, std::equal_to<>> m_ignored_dirs; 
    std::vector<std::regex> m_complex_rules;

    mutable std::shared_mutex m_mutex;
};

}  // namespace VeritasSync