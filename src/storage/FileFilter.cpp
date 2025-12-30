#include "VeritasSync/storage/FileFilter.h"

#include <fstream>
#include <iostream>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// --- 定义默认忽略规则 ---
static const std::vector<std::string> kDefaultIgnorePatterns = {".git",
                                                                ".git/",           // Git 仓库
                                                                ".veritas_tmp",    // 自身临时文件
                                                                ".veritasignore",  // 忽略文件本身
                                                                "*.part",          // 下载中间文件
                                                                ".DS_Store",
                                                                "Thumbs.db",  // 系统缩略图
                                                                "build/",
                                                                "bin/",
                                                                "out/",  // 常见构建目录
                                                                ".veritas.db",
                                                                ".veritas.db-journal",
                                                                ".veritas.db-wal",
                                                                ".veritas.db-shm"};

FileFilter::FileFilter() {
    for (const auto& pat : kDefaultIgnorePatterns) {
        classify_rule(pat);
    }
}

void FileFilter::load_rules(const std::filesystem::path& root_path) {
    std::unique_lock lock(m_mutex);

    m_ignored_exts.clear();
    m_ignored_dirs.clear();
    m_complex_rules.clear();

    for (const auto& pat : kDefaultIgnorePatterns) {
        classify_rule(pat);
    }

    std::filesystem::path ignore_file = root_path / ".veritasignore";
    if (!std::filesystem::exists(ignore_file)) return;

    std::ifstream f(ignore_file);
    if (!f.is_open()) return;

    g_logger->info("[Filter] 加载忽略规则: {}", PathToUtf8(ignore_file));

    std::string line;
    while (std::getline(f, line)) {
        // 1. 处理行尾注释：找到第一个 # 并截断
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line.erase(comment_pos);
        }

        // 2. 去除前后的空白字符 (空格、换行、制表符)
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // 3. 此时如果行为空，说明是原空行或者是纯注释行，跳过
        if (line.empty()) continue;

        classify_rule(line);
    }
}

bool FileFilter::should_ignore(std::string_view path) const {
    std::shared_lock lock(m_mutex);

    if (path.empty()) return false;
    
    // Tier 1: 后缀检查 (O(1)) - 零拷贝提取后缀
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos != std::string_view::npos) {
        std::string_view ext = path.substr(dot_pos);
        if (m_ignored_exts.count(ext)) return true;
    }

    // Tier 2: 目录前缀匹配 (修正后的逻辑)
    // 检查逻辑：路径本身匹配规则，或者路径以 "规则/" 开头
    for (const auto& dir : m_ignored_dirs) {
        // 情况 A: 路径和目录名完全相等
        if (path == dir) return true;
        // 情况 B: 路径以目录名开头，且紧跟一个 '/'
        if (path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/') return true;
    }

    // Tier 3: 复杂正则匹配 (退化路径)
    for (const auto& re : m_complex_rules) {
        // regex_match 目前还不支持 string_view，这是唯一需要转换的地方
        // 但只有极少数文件会走到这里
        if (std::regex_match(std::string(path), re)) return true;
    }

    return false;
}

static std::string glob_to_regex_internal(const std::string& glob) {
    std::string regex = "^";
    bool is_dir = false;
    std::string pattern = glob;

    if (pattern.back() == '/') {
        is_dir = true;
        pattern.pop_back();
    }

    if (pattern.find('/') == std::string::npos) {
        regex += "(.*/)?";
    } else if (pattern.front() == '/') {
        pattern.erase(0, 1);
    }

    for (char c : pattern) {
        switch (c) {
            case '*':  regex += ".*"; break;
            case '?':  regex += "."; break;
            case '.':  regex += "\\."; break;
            case '\\': regex += "/"; break;
            case '/':  regex += "/"; break;
            case '+':  regex += "\\+"; break;
            case '(':  regex += "\\("; break;
            case ')':  regex += "\\)"; break;
            case '[':  regex += "\\["; break;
            case ']':  regex += "\\]"; break;
            case '{':  regex += "\\{"; break;
            case '}':  regex += "\\}"; break;
            default:   regex += c; break;
        }
    }

    if (is_dir) {
        regex += "(/.*)?$";
    } else {
        regex += "$";
    }

    return regex;
}

void FileFilter::classify_rule(const std::string& rule) {
    if (rule.empty()) return;

    std::string pattern = rule;
    bool rooted = false;

    // 处理开头的 / (代表从根开始匹配)
    if (pattern.front() == '/') {
        pattern.erase(0, 1);
        rooted = true;
    }

    if (pattern.empty()) return;

    // 1. 简单后缀优化: *.ext
    if (pattern.size() > 2 && pattern.substr(0, 2) == "*." && 
        pattern.find_first_of("*?", 2) == std::string::npos && 
        pattern.find('/') == std::string::npos) {
        m_ignored_exts.insert(pattern.substr(1)); // 存入 ".ext"
        return;
    }

    // 2. 目录/固定路径优化 (不含通配符)
    // 根据 Git 规则：
    // - 如果包含 '/' (排除末尾)，则是路径相关的，可以放入 Tier 2
    // - 如果不包含 '/' 且没有指定 rooted ，应匹配全路径任何位置 (走 Tier 3)
    if (pattern.find_first_of("*?") == std::string::npos) {
        bool has_internal_slash = (pattern.find('/') != std::string::npos);
        
        if (rooted || has_internal_slash) {
            if (pattern.back() == '/') pattern.pop_back();
            m_ignored_dirs.insert(std::move(pattern));
            return;
        }
    }

    // 3. 复杂的、或者需要“全局匹配”的规则走正则
    try {
        m_complex_rules.emplace_back(glob_to_regex_internal(rule));
    } catch (...) {
        g_logger->warn("[Filter] 规则解析失败: {}", rule);
    }
}

}  // namespace VeritasSync