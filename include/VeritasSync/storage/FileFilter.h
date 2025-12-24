#pragma once

#include <filesystem>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace VeritasSync {

class FileFilter {
public:
    // 加载默认规则 (如 .git, .DS_Store)
    FileFilter();

    // 从指定路径加载 .veritasignore 文件
    // 如果文件不存在，则忽略
    void load_rules(const std::filesystem::path& root_path);

    // 检查路径是否应该被忽略
    // relative_path: 相对于同步根目录的路径 (例如 "src/main.cpp")
    bool should_ignore(const std::string& relative_path) const;

private:
    // 将 glob 模式 (如 "*.cpp") 转换为 regex 字符串
    std::string glob_to_regex(const std::string& glob);

    std::vector<std::regex> m_rules;
    mutable std::mutex m_mutex;
};

}  // namespace VeritasSync