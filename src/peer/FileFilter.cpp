#include "VeritasSync/FileFilter.h"

#include <fstream>
#include <iostream>

#include "VeritasSync/Logger.h"

namespace VeritasSync {

// --- 定义默认忽略规则 ---
static const std::vector<std::string> kDefaultIgnorePatterns = {
    ".git",           ".git/",             // Git 仓库
    ".veritas_tmp",                        // 自身临时文件
    ".veritasignore",                      // 忽略文件本身
    "*.part",                              // 下载中间文件
    ".DS_Store",      "Thumbs.db",         // 系统缩略图
    "build/",         "bin/",      "out/"  // 常见构建目录
};

FileFilter::FileFilter() {
    // 构造时加载默认规则
    for (const auto& pat : kDefaultIgnorePatterns) {
        try {
            m_rules.emplace_back(glob_to_regex(pat));
        } catch (...) {
        }
    }
}

void FileFilter::load_rules(const std::filesystem::path& root_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. 清空旧规则
    m_rules.clear();

    // 2. 重新加载默认规则 (替代 *this = FileFilter())
    for (const auto& pat : kDefaultIgnorePatterns) {
        try {
            m_rules.emplace_back(glob_to_regex(pat));
        } catch (...) {
        }
    }

    // 3. 加载 .veritasignore 文件
    std::filesystem::path ignore_file = root_path / ".veritasignore";
    if (!std::filesystem::exists(ignore_file)) {
        return;
    }

    std::ifstream f(ignore_file);
    if (!f.is_open()) return;

    g_logger->info("[Filter] 加载忽略规则文件: {}", ignore_file.string());

    std::string line;
    while (std::getline(f, line)) {
        // 去除空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // 跳过空行和注释
        if (line.empty() || line[0] == '#') continue;

        try {
            m_rules.emplace_back(glob_to_regex(line));
            g_logger->debug("[Filter] 添加规则: {}", line);
        } catch (const std::regex_error& e) {
            g_logger->warn("[Filter] 无效的规则 '{}': {}", line, e.what());
        }
    }
}

bool FileFilter::should_ignore(const std::string& relative_path) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 统一路径分隔符为 '/'
    std::string path = relative_path;
    std::replace(path.begin(), path.end(), '\\', '/');

    for (const auto& rule : m_rules) {
        if (std::regex_match(path, rule)) {
            return true;
        }
    }
    return false;
}

// --- 核心算法：Glob -> Regex ---
// 将 shell 通配符转换为 C++ regex
// 支持: * (任意字符), ? (单字符), / 结尾 (目录)
std::string FileFilter::glob_to_regex(const std::string& glob) {
    std::string regex = "^";  // 匹配开头
    bool is_dir = false;
    std::string pattern = glob;

    // 处理目录匹配 (如 "build/")
    if (pattern.back() == '/') {
        is_dir = true;
        pattern.pop_back();
    }

    // 如果模式不包含 '/' (如 "*.cpp")，则允许匹配任意层级
    if (pattern.find('/') == std::string::npos) {
        regex += "(.*/)?";
    } else if (pattern.front() == '/') {
        // 如果以 / 开头 (如 "/src")，去掉它，表示从根匹配
        pattern.erase(0, 1);
    }

    for (char c : pattern) {
        switch (c) {
            case '*':
                regex += ".*";
                break;
            case '?':
                regex += ".";
                break;
            case '.':
                regex += "\\.";
                break;
            case '\\':
                regex += "/";
                break;  // 统一转义
            case '/':
                regex += "/";
                break;
            case '+':
                regex += "\\+";
                break;
            case '(':
                regex += "\\(";
                break;
            case ')':
                regex += "\\)";
                break;
            case '[':
                regex += "\\[";
                break;
            case ']':
                regex += "\\]";
                break;
            case '{':
                regex += "\\{";
                break;
            case '}':
                regex += "\\}";
                break;
            default:
                regex += c;
                break;
        }
    }

    // 如果规则以 / 结尾 (目录模式)，则匹配该目录及其下所有内容
    if (is_dir) {
        regex += "(/.*)?$";
    } else {
        regex += "$";  // 精确匹配文件名
    }

    return regex;
}

}  // namespace VeritasSync