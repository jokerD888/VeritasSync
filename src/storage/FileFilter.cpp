#include "VeritasSync/storage/FileFilter.h"

#include <fstream>
#include <iostream>
#include <sstream>

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
        parse_rule(pat, /*is_default=*/true);
    }
}

void FileFilter::load_rules(const std::filesystem::path& root_path) {
    std::unique_lock lock(m_mutex);

    // 清除所有状态
    m_default_fast.exts.clear();
    m_default_fast.dirs.clear();
    m_default_complex.clear();
    m_user_rules.clear();
    m_user_fast.exts.clear();
    m_user_fast.dirs.clear();
    m_has_negation = false;

    // 重新加载默认规则
    for (const auto& pat : kDefaultIgnorePatterns) {
        parse_rule(pat, /*is_default=*/true);
    }

    std::filesystem::path ignore_file = root_path / ".veritasignore";
    if (!std::filesystem::exists(ignore_file)) return;

    std::ifstream f(ignore_file);
    if (!f.is_open()) return;

    g_logger->info("[Filter] 加载忽略规则: {}", PathToUtf8(ignore_file));

    std::string line;
    while (std::getline(f, line)) {
        // 去除行尾 \r (跨平台兼容)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 先 trim 前导空白，方便判断行首字符
        size_t line_start = line.find_first_not_of(" \t");
        if (line_start == std::string::npos) continue;

        // 处理转义和注释：
        // P3: 支持 \# 和 \! 转义
        // 规则：如果 trim 后行首是 \# 或 \!，将 \ 去掉，# / ! 是字面量
        // 对于行中的 #，如果前面没有反斜杠，截断为注释
        //
        // 关键：如果行首的 ! 来自 \! 转义，则不应被 parse_rule 当作取反规则。
        // 我们通过检测行首是否是 \! 来判断，如果是则在 processed 结果前插入
        // 一个特殊前缀 "\x01"，parse_rule 会识别并跳过取反检测。
        std::string processed;
        processed.reserve(line.size());
        bool leading_bang_escaped = false;  // 行首的 ! 是否来自 \! 转义

        for (size_t i = line_start; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                char next = line[i + 1];
                if (next == '#' || next == '!' || next == '\\') {
                    // 转义字符：\#, \!, \\ → 输出字面量
                    if (next == '!' && processed.empty()) {
                        leading_bang_escaped = true;
                    }
                    processed += next;
                    ++i;  // 跳过下一个字符
                    continue;
                }
            }
            if (line[i] == '#') {
                // 未转义的 # → 截断为注释
                break;
            }
            processed += line[i];
        }

        // 去除尾部空白
        size_t end = processed.find_last_not_of(" \t");
        if (end == std::string::npos) continue;
        processed = processed.substr(0, end + 1);

        if (processed.empty()) continue;

        // 如果行首 ! 来自转义 \!，则告知 parse_rule 这不是取反规则
        parse_rule(processed, /*is_default=*/false, leading_bang_escaped);
    }
}

void FileFilter::load_rules_from_string(const std::string& rules) {
    std::unique_lock lock(m_mutex);

    // 重置状态
    m_default_fast.exts.clear();
    m_default_fast.dirs.clear();
    m_default_complex.clear();
    m_user_rules.clear();
    m_user_fast.exts.clear();
    m_user_fast.dirs.clear();
    m_has_negation = false;

    // 加载默认规则
    for (const auto& pat : kDefaultIgnorePatterns) {
        parse_rule(pat, /*is_default=*/true);
    }

    // 逐行解析用户规则
    std::istringstream iss(rules);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t line_start = line.find_first_not_of(" \t");
        if (line_start == std::string::npos) continue;

        // 简化处理（不含转义逻辑，dry-run 足够）
        std::string trimmed = line.substr(line_start);
        size_t comment_pos = trimmed.find('#');
        if (comment_pos == 0) continue;  // 注释行
        if (comment_pos != std::string::npos) trimmed = trimmed.substr(0, comment_pos);

        // 去除尾部空白
        size_t end = trimmed.find_last_not_of(" \t");
        if (end == std::string::npos) continue;
        trimmed = trimmed.substr(0, end + 1);

        if (!trimmed.empty()) {
            parse_rule(trimmed);
        }
    }
}

bool FileFilter::should_ignore(std::string_view path) const {
    std::shared_lock lock(m_mutex);

    if (path.empty()) return false;

    // === 阶段 1: 默认规则快速路径 (不可被取反覆盖) ===

    // Tier 1: 后缀检查 (O(1))
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos != std::string_view::npos) {
        std::string_view ext = path.substr(dot_pos);
        if (m_default_fast.exts.count(ext)) return true;
    }

    // Tier 2: 目录前缀匹配
    for (const auto& dir : m_default_fast.dirs) {
        if (path == dir) return true;
        if (path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/')
            return true;
    }

    // Tier 3: 默认复杂规则
    for (const auto& re : m_default_complex) {
        if (std::regex_match(std::string(path), re)) return true;
    }

    // === 阶段 2: 用户规则 ===

    // 快速路径：如果没有取反规则，可以使用快速匹配
    if (!m_has_negation) {
        // 后缀检查
        if (dot_pos != std::string_view::npos) {
            std::string_view ext = path.substr(dot_pos);
            if (m_user_fast.exts.count(ext)) return true;
        }
        // 目录前缀
        for (const auto& dir : m_user_fast.dirs) {
            if (path == dir) return true;
            if (path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/')
                return true;
        }
        // 复杂正则
        for (const auto& rule : m_user_rules) {
            if (std::regex_match(std::string(path), rule.regex)) return true;
        }
        return false;
    }

    // 有取反规则：必须按顺序逐条评估，最后一条匹配的规则决定结果
    // gitignore 语义：最后匹配的规则胜出
    bool ignored = false;
    for (const auto& rule : m_user_rules) {
        if (std::regex_match(std::string(path), rule.regex)) {
            ignored = !rule.negated;  // 普通规则 → 忽略；取反规则 → 不忽略
        }
    }

    return ignored;
}

// 将 glob 模式转换为正则表达式
// 支持: *, ?, **, [abc], [a-z], [!abc], \#, \!, 目录斜杠, 根锚定
std::string FileFilter::glob_to_regex(const std::string& glob) {
    std::string regex = "^";
    bool is_dir = false;
    std::string pattern = glob;

    // 检测并移除尾部 / (目录标记)
    if (!pattern.empty() && pattern.back() == '/') {
        is_dir = true;
        pattern.pop_back();
    }

    // 锚定逻辑：
    // - 如果模式包含 / (排除尾部的 /)，则从根开始匹配
    // - 如果模式以 / 开头，去掉前导 /，从根开始匹配
    // - 否则匹配任意路径位置
    bool rooted = false;
    if (!pattern.empty() && pattern.front() == '/') {
        rooted = true;
        pattern.erase(0, 1);
    } else if (pattern.find('/') != std::string::npos) {
        rooted = true;
    }

    if (!rooted) {
        regex += "(.*/)?";
    }

    // 逐字符转换
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];

        // P1: ** 双星号处理
        if (c == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
            // ** 匹配任意数量的目录层级（包括零层）
            if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                // **/ → 匹配零个或多个目录前缀
                regex += "(.*/)?" ;
                i += 2;  // 跳过 **/ 
            } else if (i == 0 || (i > 0 && pattern[i - 1] == '/')) {
                // 末尾的 ** 或 /**  → 匹配任意子路径
                regex += ".*";
                i += 1;  // 跳过第二个 *
            } else {
                // 不在 / 边界上的 **，当作两个 * 处理 → .*
                regex += ".*";
                i += 1;
            }
            continue;
        }

        switch (c) {
            case '*':
                // 单个 * 匹配除 / 以外的任意字符序列
                regex += "[^/]*";
                break;
            case '?':
                // ? 匹配除 / 以外的单个字符
                regex += "[^/]";
                break;
            case '[': {
                // P2: 字符类 [abc] [a-z] [!abc]
                // 找到匹配的 ]
                size_t close = pattern.find(']', i + 1);
                if (close == std::string::npos) {
                    // 没有闭合的 ]，当作字面量
                    regex += "\\[";
                    break;
                }
                regex += '[';
                size_t j = i + 1;
                // 支持 [!...] 取反字符类 (gitignore 语法，等价于正则的 [^...])
                if (j < close && pattern[j] == '!') {
                    regex += '^';
                    ++j;
                }
                // 复制字符类内容，转义正则特殊字符（但保留 - 用于范围）
                for (; j < close; ++j) {
                    char cc = pattern[j];
                    if (cc == '\\') {
                        regex += "\\\\";
                    } else {
                        regex += cc;
                    }
                }
                regex += ']';
                i = close;  // 跳过到 ]
                break;
            }
            case '.':
                regex += "\\.";
                break;
            case '\\':
                // 规则文件中的反斜杠：在 Windows 路径中当作 /
                // 但如果是转义序列（如 \# \!），在 load_rules 中已经处理过了
                regex += "/";
                break;
            case '/':
                regex += "/";
                break;
            // 转义正则特殊字符
            case '+':  regex += "\\+"; break;
            case '(':  regex += "\\("; break;
            case ')':  regex += "\\)"; break;
            case '{':  regex += "\\{"; break;
            case '}':  regex += "\\}"; break;
            case '^':  regex += "\\^"; break;
            case '$':  regex += "\\$"; break;
            case '|':  regex += "\\|"; break;
            default:
                regex += c;
                break;
        }
    }

    if (is_dir) {
        regex += "(/.*)?$";
    } else {
        regex += "$";
    }

    return regex;
}

void FileFilter::parse_rule(const std::string& raw_rule, bool is_default, bool bang_escaped) {
    if (raw_rule.empty()) return;

    std::string rule = raw_rule;
    bool negated = false;

    // P0: 检测 ! 前缀 (取反规则)
    // 如果 bang_escaped 为 true，说明行首的 ! 来自 \! 转义，不是取反规则
    if (!is_default && !bang_escaped && rule.front() == '!') {
        negated = true;
        rule.erase(0, 1);
        if (rule.empty()) return;
        m_has_negation = true;
    }

    // === 默认规则：走快速路径 ===
    if (is_default) {
        std::string pattern = rule;
        bool rooted = false;

        if (!pattern.empty() && pattern.front() == '/') {
            pattern.erase(0, 1);
            rooted = true;
        }
        if (pattern.empty()) return;

        // 尝试后缀优化: *.ext
        if (pattern.size() > 2 && pattern.substr(0, 2) == "*." &&
            pattern.find_first_of("*?[", 2) == std::string::npos &&
            pattern.find('/') == std::string::npos) {
            m_default_fast.exts.insert(pattern.substr(1));  // 存入 ".ext"
            return;
        }

        // 尝试目录/固定路径优化
        if (pattern.find_first_of("*?[") == std::string::npos) {
            bool has_internal_slash = (pattern.find('/') != std::string::npos);
            if (rooted || has_internal_slash) {
                if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
                m_default_fast.dirs.insert(std::move(pattern));
                return;
            }
        }

        // 复杂规则 → 正则
        try {
            m_default_complex.emplace_back(glob_to_regex(rule));
        } catch (...) {
            g_logger->warn("[Filter] 默认规则解析失败: {}", rule);
        }
        return;
    }

    // === 用户规则 ===

    // 如果没有取反规则，尝试填充快速路径缓存（加速无取反场景）
    if (!negated && !m_has_negation) {
        std::string pattern = rule;
        bool rooted = false;

        if (!pattern.empty() && pattern.front() == '/') {
            pattern.erase(0, 1);
            rooted = true;
        }

        if (!pattern.empty()) {
            // 后缀优化: *.ext
            if (pattern.size() > 2 && pattern.substr(0, 2) == "*." &&
                pattern.find_first_of("*?[", 2) == std::string::npos &&
                pattern.find('/') == std::string::npos) {
                m_user_fast.exts.insert(pattern.substr(1));
            }

            // 目录/固定路径优化
            if (pattern.find_first_of("*?[") == std::string::npos) {
                bool has_internal_slash = (pattern.find('/') != std::string::npos);
                if (rooted || has_internal_slash) {
                    std::string dir_pattern = pattern;
                    if (!dir_pattern.empty() && dir_pattern.back() == '/')
                        dir_pattern.pop_back();
                    m_user_fast.dirs.insert(dir_pattern);
                }
            }
        }
    }

    // 所有用户规则都加入有序列表（支持取反逻辑）
    try {
        CompiledRule compiled;
        compiled.regex = std::regex(glob_to_regex(rule));
        compiled.negated = negated;
        compiled.original = raw_rule;
        m_user_rules.push_back(std::move(compiled));
    } catch (...) {
        g_logger->warn("[Filter] 用户规则解析失败: {}", raw_rule);
    }
}

}  // namespace VeritasSync
