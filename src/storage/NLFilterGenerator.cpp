#include "VeritasSync/storage/NLFilterGenerator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Logger.h"
#include "VeritasSync/storage/FileFilter.h"

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// LLM 配置管理
// ═══════════════════════════════════════════════════════════════

void NLFilterGenerator::set_llm_config(const LLMConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_llm_config = config;
    m_has_llm = !config.api_url.empty() && !config.api_key.empty();
    if (g_logger) {
        if (m_has_llm) {
            g_logger->info("[NLFilter] LLM 已配置: url={}, model={}", config.api_url, config.model);
        } else {
            g_logger->info("[NLFilter] LLM 未配置，仅使用模板引擎");
        }
    }
}

bool NLFilterGenerator::has_llm() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_has_llm;
}

// ═══════════════════════════════════════════════════════════════
// 核心生成接口
// ═══════════════════════════════════════════════════════════════

NLFilterGenerator::Result NLFilterGenerator::generate(
    const std::string& description, const std::string& existing_rules,
    const std::string& sync_folder) const {

    if (!has_llm()) {
        Result result;
        result.success = false;
        result.error = "未配置 AI API Key。请在设置页面或 config.json 中填写 llm_api_key 以启用 AI 智能生成。";
        return result;
    }

    // RAG：扫描同步目录生成结构摘要，让 LLM 了解实际项目结构
    std::string dir_summary;
    std::string file_samples;
    LLMConfig llm_cfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        llm_cfg = m_llm_config;
    }

    if (!sync_folder.empty()) {
        dir_summary = build_directory_summary(sync_folder, 3000,
                                              llm_cfg.max_scan_files, llm_cfg.large_dir_threshold);
        file_samples = search_relevant_samples(sync_folder, description, 10, llm_cfg.max_scan_files);
        if (g_logger) {
            g_logger->info("[NLFilter] RAG 上下文: 目录摘要 {} 字符, 文件样本 {} 字符",
                           dir_summary.size(), file_samples.size());
        }
    }

    auto result = generate_from_llm(description, existing_rules, dir_summary + file_samples);

    // Dry-Run：在本地文件树上试跑规则，统计将被忽略的文件数
    if (result.success && !sync_folder.empty()) {
        result.affected_files = dry_run_rules(sync_folder, result.rules, llm_cfg.max_scan_files);
        if (g_logger) {
            g_logger->info("[NLFilter] Dry-Run: 规则将影响 {} 个文件", result.affected_files);
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// LLM API 调用
// ═══════════════════════════════════════════════════════════════

NLFilterGenerator::Result NLFilterGenerator::generate_from_llm(
    const std::string& description, const std::string& existing_rules,
    const std::string& dir_summary) const {
    
    Result result;
    LLMConfig config;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_has_llm) {
            result.success = false;
            result.error = "LLM API 未配置";
            return result;
        }
        config = m_llm_config;
    }

    try {
        // 解析 URL
        std::string scheme, host, path;
        int port = -1;

        // 解析 scheme
        size_t scheme_end = config.api_url.find("://");
        if (scheme_end != std::string::npos) {
            scheme = config.api_url.substr(0, scheme_end);
            std::string rest = config.api_url.substr(scheme_end + 3);
            
            // 解析 host:port 和 path
            size_t path_start = rest.find('/');
            std::string host_port;
            if (path_start != std::string::npos) {
                host_port = rest.substr(0, path_start);
                path = rest.substr(path_start);
            } else {
                host_port = rest;
                path = "/v1/chat/completions";  // 默认路径
            }

            // 解析 port
            size_t colon = host_port.rfind(':');
            if (colon != std::string::npos) {
                host = host_port.substr(0, colon);
                port = std::stoi(host_port.substr(colon + 1));
            } else {
                host = host_port;
            }
        } else {
            result.success = false;
            result.error = "无效的 API URL: " + config.api_url;
            return result;
        }

        // 构造基础 URL（scheme://host[:port]）
        std::string base_url = scheme + "://" + host;
        if (port > 0) {
            base_url += ":" + std::to_string(port);
        }

        // 确保 path 以 /v1/chat/completions 结尾（如果用户只给了域名）
        if (path.empty() || path == "/") {
            path = "/v1/chat/completions";
        }

        // 创建 HTTP 客户端
        httplib::Client cli(base_url);
        cli.set_connection_timeout(config.timeout_seconds);
        cli.set_read_timeout(config.timeout_seconds);
        cli.set_bearer_token_auth(config.api_key);

        // 构建请求体
        std::string prompt = build_llm_prompt(description, existing_rules, dir_summary);
        nlohmann::json request_body = {
            {"model", config.model.empty() ? "qwen-turbo" : config.model},
            {"messages", {{
                {"role", "user"},
                {"content", prompt}
            }}},
            {"temperature", config.temperature},
            {"max_tokens", config.max_tokens},
            {"response_format", {{"type", "json_object"}}}
        };

        if (g_logger) {
            g_logger->info("[NLFilter] 调用 LLM API: {} {}", base_url, path);
        }

        auto res = cli.Post(path, request_body.dump(), "application/json");

        if (!res) {
            result.success = false;
            result.error = "LLM API 请求失败: 网络错误或超时";
            if (g_logger) g_logger->error("[NLFilter] LLM API 请求失败: {}", httplib::to_string(res.error()));
            return result;
        }

        if (res->status != 200) {
            result.success = false;
            result.error = "LLM API 返回错误 (HTTP " + std::to_string(res->status) + "): " + res->body;
            if (g_logger) g_logger->error("[NLFilter] LLM API 错误: status={}, body={}", res->status, res->body);
            return result;
        }

        // 解析响应
        auto response_json = nlohmann::json::parse(res->body);
        std::string content;
        
        // 兼容 OpenAI / DeepSeek / 通义 等格式
        if (response_json.contains("choices") && !response_json["choices"].empty()) {
            content = response_json["choices"][0]["message"]["content"].get<std::string>();
        } else if (response_json.contains("output")) {
            // 通义千问格式
            content = response_json["output"]["text"].get<std::string>();
        } else {
            result.success = false;
            result.error = "无法解析 LLM 响应格式";
            return result;
        }

        // 提取规则（JSON 结构化解析）
        auto parsed = extract_rules_from_response(content);
        if (parsed.rules.empty()) {
            result.success = false;
            result.error = "LLM 未生成有效的过滤规则";
            return result;
        }

        // 验证规则合法性
        if (!validate_rules(parsed.rules)) {
            result.success = false;
            result.error = "LLM 生成的规则格式无效";
            return result;
        }

        result.success = true;
        result.rules = parsed.rules;
        result.explanation = parsed.explanation;
        result.source = "llm";

        if (g_logger) {
            g_logger->info("[NLFilter] LLM 生成成功: 输入=\"{}\", 规则行数={}",
                description, std::count(parsed.rules.begin(), parsed.rules.end(), '\n') + 1);
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("LLM API 调用异常: ") + e.what();
        if (g_logger) g_logger->error("[NLFilter] LLM 异常: {}", e.what());
    }

    return result;
}

std::string NLFilterGenerator::build_llm_prompt(
    const std::string& description, const std::string& existing_rules,
    const std::string& dir_summary) {

    std::string prompt = R"(你是一个文件同步工具的过滤规则生成助手。用户会用自然语言描述想要忽略的文件，你需要生成 .gitignore 兼容格式的过滤规则。

## 规则语法（.gitignore 兼容）：
- `*` 匹配除 `/` 以外的任意字符序列（不跨目录）
- `?` 匹配除 `/` 以外的单个字符
- `**/` 匹配零个或多个目录层级
- `[abc]` 字符类匹配
- 以 `/` 结尾表示只匹配目录
- 以 `!` 开头表示取反（白名单），不忽略该文件
- `#` 开头表示注释
- 前导 `/` 表示从根目录开始匹配

## 输出格式要求（最高优先级）：
1. 你必须且只能输出一个合法的 JSON 对象
2. 绝对不要输出任何解释性文字
3. JSON 结构如下：
{"rules": ["第一条规则", "第二条规则"], "explanation": "简要说明"}
4. rules 数组中每个元素是一条独立的忽略规则
5. 不要重复已有的规则
6. 根据项目实际目录结构生成精准规则，不要生成项目中不存在的目录或文件类型的规则

)";

    if (!dir_summary.empty()) {
        prompt += "## 项目目录结构：\n```\n" + dir_summary + "\n```\n\n";
    }

    if (!existing_rules.empty()) {
        prompt += "## 用户已有的规则：\n```\n" + existing_rules + "\n```\n\n";
    }

    prompt += "## 用户需求：\n" + description + "\n\n请生成对应的过滤规则：";

    return prompt;
}

NLFilterGenerator::ParsedResponse NLFilterGenerator::extract_rules_from_response(const std::string& response) {
    ParsedResponse parsed;

    // === JSON 大括号提取法（Brace Extraction）===
    // 无论 LLM 在 JSON 前后加了什么废话，{ } 的边界是确定的
    size_t start = response.find_first_of('{');
    size_t end = response.find_last_of('}');

    if (start != std::string::npos && end != std::string::npos && start < end) {
        std::string json_str = response.substr(start, end - start + 1);

        try {
            auto j = nlohmann::json::parse(json_str);

            // 提取 rules 数组
            if (j.contains("rules") && j["rules"].is_array()) {
                std::ostringstream oss;
                for (const auto& item : j["rules"]) {
                    if (item.is_string()) {
                        std::string rule = item.get<std::string>();
                        if (!rule.empty()) {
                            if (!oss.str().empty()) oss << "\n";
                            oss << rule;
                        }
                    }
                }
                parsed.rules = oss.str();
            }

            // 提取 explanation
            if (j.contains("explanation") && j["explanation"].is_string()) {
                parsed.explanation = j["explanation"].get<std::string>();
            }

            if (!parsed.rules.empty()) return parsed;
        } catch (const nlohmann::json::parse_error&) {
            // JSON 解析失败，落入下方 fallback
            if (g_logger) g_logger->warn("[NLFilter] JSON 解析失败，尝试 fallback 提取");
        }
    }

    // === Fallback：逐行过滤 ===
    // 某些旧模型不支持 json_object 模式，可能输出纯文本
    std::istringstream iss(response);
    std::string line;
    std::ostringstream cleaned;
    while (std::getline(iss, line)) {
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        char first = line[s];
        // 规则行特征：以 * . / ! # [ 或 ASCII 字母开头，且不含中文
        if (first == '*' || first == '.' || first == '/' || first == '!' ||
            first == '#' || first == '[' || std::isalpha(static_cast<unsigned char>(first))) {
            bool has_multibyte = false;
            for (size_t i = s; i < line.size() && i < s + 30; ++i) {
                if (static_cast<unsigned char>(line[i]) > 0x7F) { has_multibyte = true; break; }
            }
            if (!has_multibyte) {
                if (!cleaned.str().empty()) cleaned << "\n";
                cleaned << line.substr(s);
            }
        }
    }
    parsed.rules = cleaned.str();
    if (parsed.explanation.empty()) {
        parsed.explanation = "由 AI 根据描述自动生成";
    }

    return parsed;
}

bool NLFilterGenerator::validate_rules(const std::string& rules) {
    // 逐行验证：每行要么是注释，要么是空行，要么是合法的 glob 模式
    std::istringstream iss(rules);
    std::string line;
    int valid_rules = 0;

    while (std::getline(iss, line)) {
        // 去除首尾空白
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;
        
        std::string trimmed = line.substr(start, end - start);

        // 空行或注释行，跳过
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // 去除可能的取反前缀
        std::string pattern = trimmed;
        if (!pattern.empty() && pattern[0] == '!') {
            pattern = pattern.substr(1);
        }

        // 基本合法性检查：不应包含某些危险字符序列
        if (pattern.find("..") == 0) return false;  // 禁止 ../ 开头
        if (pattern.empty()) continue;

        ++valid_rules;
    }

    return valid_rules > 0;
}

// ═══════════════════════════════════════════════════════════════
// RAG：目录结构摘要生成
// ═══════════════════════════════════════════════════════════════

std::string NLFilterGenerator::build_directory_summary(const std::string& root_path, size_t max_chars,
                                                       size_t max_scan_files, size_t large_dir_threshold) {
    namespace fs = std::filesystem;

    // 按目录聚合：每个目录的文件数量和扩展名集合
    struct DirInfo {
        size_t file_count = 0;
        std::map<std::string, size_t> ext_counts;  // 扩展名 → 数量
    };
    std::map<std::string, DirInfo> dirs;  // 相对目录路径 → 信息

    try {
        fs::path root(root_path);
        if (!fs::exists(root) || !fs::is_directory(root)) return "";

        size_t total_files = 0;
        const size_t MAX_SCAN_FILES = max_scan_files;  // 防止超大目录卡住

        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {

            if (total_files >= MAX_SCAN_FILES) break;

            // 跳过 .git 和 .veritas.db 等内部文件
            const auto& p = it->path();
            std::string filename = p.filename().string();
            if (filename == ".git" || filename == ".veritas.db") {
                it.disable_recursion_pending();
                continue;
            }

            if (!it->is_regular_file()) continue;
            ++total_files;

            // 计算相对目录路径
            auto rel = fs::relative(p.parent_path(), root).string();
            if (rel == ".") rel = "(root)";
            // 统一路径分隔符
            std::replace(rel.begin(), rel.end(), '\\', '/');

            // 提取扩展名
            // 注意：.gitignore 等以点开头的隐藏文件，extension() 行为不一致
            // 有些平台返回 "" (整个是 stem)，有些返回 ".gitignore" (整个是 extension)
            std::string fname_str = p.filename().string();
            std::string ext;
            if (fname_str.size() > 1 && fname_str[0] == '.') {
                // 以点开头的隐藏文件：如果除了前导点之外还有扩展名则取之，否则归入 (dotfile)
                auto dot_pos = fname_str.find('.', 1);
                ext = (dot_pos != std::string::npos) ? fname_str.substr(dot_pos) : "(dotfile)";
            } else {
                ext = p.extension().string();
                if (ext.empty()) ext = "(no ext)";
            }

            dirs[rel].file_count++;
            dirs[rel].ext_counts[ext]++;
        }

        if (dirs.empty()) return "(empty directory)";

        // 输出策略：浅层目录优先，大目录折叠
        // 按目录深度排序（浅层优先），同深度按文件数降序
        std::vector<std::pair<std::string, DirInfo>> sorted_dirs(dirs.begin(), dirs.end());
        std::sort(sorted_dirs.begin(), sorted_dirs.end(),
            [](const auto& a, const auto& b) {
                // 计算路径深度（/ 的数量）
                auto depth = [](const std::string& s) {
                    return std::count(s.begin(), s.end(), '/');
                };
                int da = depth(a.first), db = depth(b.first);
                if (da != db) return da < db;  // 浅层优先
                return a.second.file_count > b.second.file_count;  // 同深度按文件数降序
            });

        // 格式化输出
        std::ostringstream oss;
        oss << "Total: " << total_files << " files";
        if (total_files >= MAX_SCAN_FILES) oss << "+ (truncated)";
        oss << "\n\n";

        const size_t LARGE_DIR_THRESHOLD = large_dir_threshold;  // 超过此数量的目录折叠显示

        for (const auto& [dir, info] : sorted_dirs) {
            // 检查长度限制
            if (oss.str().size() > max_chars) {
                oss << "... (truncated, too many directories)\n";
                break;
            }

            // 大目录折叠：只显示一行摘要，不列扩展名细节
            if (info.file_count > LARGE_DIR_THRESHOLD) {
                oss << dir << "/  " << info.file_count << " files [large dir]\n";
                continue;
            }

            oss << dir << "/  " << info.file_count << " files (";

            // 按数量降序列出扩展名
            std::vector<std::pair<size_t, std::string>> sorted_exts;
            for (const auto& [ext, cnt] : info.ext_counts) {
                sorted_exts.emplace_back(cnt, ext);
            }
            std::sort(sorted_exts.rbegin(), sorted_exts.rend());

            // 最多显示 5 种扩展名
            size_t shown = 0;
            for (const auto& [cnt, ext] : sorted_exts) {
                if (shown > 0) oss << ", ";
                if (shown >= 5) { oss << "..."; break; }
                oss << ext;
                if (cnt > 1) oss << "×" << cnt;
                ++shown;
            }
            oss << ")\n";
        }

        return oss.str();

    } catch (const std::exception& e) {
        if (g_logger) g_logger->warn("[NLFilter] 扫描目录失败: {}", e.what());
        return "";
    }
}

// ═══════════════════════════════════════════════════════════════
// RAG：关键词采样 — 从用户描述中提取关键词，搜索匹配文件样本
// ═══════════════════════════════════════════════════════════════

std::string NLFilterGenerator::search_relevant_samples(
    const std::string& root_path, const std::string& description, size_t max_samples,
    size_t max_scan_files) {
    namespace fs = std::filesystem;

    // 1. 从用户描述中提取搜索关键词
    //    常见模式：test, build, log, cache, temp, doc, image, video, config 等
    //    同时处理中文关键词到英文的映射
    static const std::vector<std::pair<std::string, std::vector<std::string>>> keyword_map = {
        {"test",   {"test", "测试", "spec", "mock"}},
        {"build",  {"build", "编译", "构建", "dist", "output", "产物"}},
        {"log",    {"log", "日志"}},
        {"cache",  {"cache", "缓存"}},
        {"temp",   {"temp", "tmp", "临时"}},
        {"doc",    {"doc", "文档", "readme"}},
        {"config", {"config", "配置", "conf", "setting"}},
        {"image",  {"image", "图片", "img", "图像", "照片"}},
        {"video",  {"video", "视频"}},
        {"font",   {"font", "字体"}},
    };

    // 小写化描述
    std::string desc_lower;
    desc_lower.reserve(description.size());
    for (unsigned char c : description) {
        if (c >= 'A' && c <= 'Z') desc_lower += static_cast<char>(c + 32);
        else desc_lower += static_cast<char>(c);
    }

    // 收集匹配的搜索关键词
    std::vector<std::string> search_terms;
    for (const auto& [term, triggers] : keyword_map) {
        for (const auto& trigger : triggers) {
            if (desc_lower.find(trigger) != std::string::npos) {
                search_terms.push_back(term);
                break;
            }
        }
    }

    // 如果没有匹配到预定义关键词，把用户描述中的英文单词都作为搜索词
    if (search_terms.empty()) {
        std::string word;
        for (char c : desc_lower) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
                word += c;
            } else {
                if (word.size() >= 3) search_terms.push_back(word);
                word.clear();
            }
        }
        if (word.size() >= 3) search_terms.push_back(word);
    }

    if (search_terms.empty()) return "";

    // 2. 扫描文件树，搜索匹配的路径
    std::vector<std::string> matched_paths;
    std::vector<std::string> unmatched_siblings;  // 同目录下不匹配的文件（用于对比）

    try {
        fs::path root(root_path);
        if (!fs::exists(root)) return "";

        const size_t MAX_SCAN = (max_scan_files > 0) ? max_scan_files : 50000;
        size_t scanned = 0;
        std::set<std::string> matched_dirs;  // 记录匹配文件所在的目录

        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {

            if (scanned >= MAX_SCAN) break;

            const auto& p = it->path();
            std::string fname = p.filename().string();
            if (fname == ".git" || fname == ".veritas.db") {
                it.disable_recursion_pending();
                continue;
            }

            if (!it->is_regular_file()) continue;
            ++scanned;

            auto rel = fs::relative(p, root).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');

            // 小写化路径用于匹配
            std::string rel_lower;
            rel_lower.reserve(rel.size());
            for (unsigned char c : rel) {
                rel_lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
            }

            // 检查路径是否包含任一搜索词
            bool matches = false;
            for (const auto& term : search_terms) {
                if (rel_lower.find(term) != std::string::npos) {
                    matches = true;
                    break;
                }
            }

            if (matches && matched_paths.size() < max_samples) {
                matched_paths.push_back(rel);
                // 记录所在目录
                auto parent = fs::relative(p.parent_path(), root).string();
                std::replace(parent.begin(), parent.end(), '\\', '/');
                matched_dirs.insert(parent);
            }
        }

        // 3. 从匹配文件所在的目录中找几个不匹配的文件（给 LLM 做对比参考）
        if (!matched_dirs.empty()) {
            for (auto it = fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {

                if (!it->is_regular_file()) continue;

                auto parent = fs::relative(it->path().parent_path(), root).string();
                std::replace(parent.begin(), parent.end(), '\\', '/');

                if (matched_dirs.count(parent)) {
                    auto rel = fs::relative(it->path(), root).string();
                    std::replace(rel.begin(), rel.end(), '\\', '/');

                    // 确认这个文件不在匹配列表中
                    bool is_matched = false;
                    for (const auto& m : matched_paths) {
                        if (m == rel) { is_matched = true; break; }
                    }

                    if (!is_matched && unmatched_siblings.size() < 5) {
                        unmatched_siblings.push_back(rel);
                    }
                }

                if (unmatched_siblings.size() >= 5) break;
            }
        }

    } catch (const std::exception& e) {
        if (g_logger) g_logger->warn("[NLFilter] 文件采样失败: {}", e.what());
        return "";
    }

    if (matched_paths.empty()) return "";

    // 4. 格式化输出
    std::ostringstream oss;
    oss << "\n## 与用户需求相关的文件样本（本地搜索 \"";
    for (size_t i = 0; i < search_terms.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << search_terms[i];
    }
    oss << "\"）：\n";

    oss << "匹配的文件 (应被忽略规则覆盖):\n";
    for (const auto& p : matched_paths) {
        oss << "  ✓ " << p << "\n";
    }

    if (!unmatched_siblings.empty()) {
        oss << "同目录下不匹配的文件 (不应被误伤):\n";
        for (const auto& p : unmatched_siblings) {
            oss << "  ✗ " << p << "\n";
        }
    }

    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// Dry-Run：用生成的规则在文件树上试跑
// ═══════════════════════════════════════════════════════════════

size_t NLFilterGenerator::dry_run_rules(const std::string& root_path, const std::string& rules,
                                        size_t max_scan_files) {
    namespace fs = std::filesystem;

    try {
        fs::path root(root_path);
        if (!fs::exists(root)) return 0;

        // 用 FileFilter 加载规则（纯内存操作，不读写文件）
        FileFilter filter;
        filter.load_rules_from_string(rules);

        // 遍历文件树，统计匹配数量
        size_t count = 0;
        const size_t MAX_SCAN = (max_scan_files > 0) ? max_scan_files : 50000;
        size_t scanned = 0;

        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {

            if (scanned >= MAX_SCAN) break;

            const auto& p = it->path();
            std::string fname = p.filename().string();
            if (fname == ".git" || fname == ".veritas.db") {
                it.disable_recursion_pending();
                continue;
            }

            if (!it->is_regular_file()) continue;
            ++scanned;

            auto rel = fs::relative(p, root).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');

            if (filter.should_ignore(rel)) {
                ++count;
            }
        }

        return count;

    } catch (const std::exception& e) {
        if (g_logger) g_logger->warn("[NLFilter] Dry-run 失败: {}", e.what());
        return 0;
    }
}

}  // namespace VeritasSync
