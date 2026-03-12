#include "VeritasSync/storage/NLFilterGenerator.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "VeritasSync/common/Logger.h"

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
    const std::string& description, const std::string& existing_rules) const {
    
    // 第一层：尝试模板匹配
    auto result = generate_from_template(description);
    if (result.success) {
        return result;
    }

    // 第二层：回退到 LLM
    if (has_llm()) {
        return generate_from_llm(description, existing_rules);
    }

    // 两层都失败
    result.success = false;
    result.error = "无法理解该描述。请尝试更具体的表述，例如：\n"
                   "  · \"忽略所有日志文件\"\n"
                   "  · \"忽略 build 目录\"\n"
                   "  · \"忽略所有图片文件\"\n"
                   "  · \"忽略 node_modules 和 .git 目录\"";
    return result;
}

// ═══════════════════════════════════════════════════════════════
// 第一层：模板引擎
// ═══════════════════════════════════════════════════════════════

NLFilterGenerator::Result NLFilterGenerator::generate_from_template(const std::string& description) const {
    std::string normalized = normalize(description);
    
    Result result;
    std::string combined_rules;
    std::string combined_explanation;
    int match_count = 0;

    const auto& templates = get_templates();

    // 遍历所有模板，收集所有匹配的规则
    for (const auto& tmpl : templates) {
        for (const auto& keyword : tmpl.keywords) {
            if (contains_keyword(normalized, keyword)) {
                if (!combined_rules.empty()) {
                    combined_rules += "\n";
                    combined_explanation += "；";
                }
                combined_rules += tmpl.rules;
                combined_explanation += tmpl.explanation;
                ++match_count;
                break;  // 每个模板最多匹配一次
            }
        }
    }

    // 处理取反（白名单）意图
    auto exceptions = extract_exceptions(normalized);
    for (const auto& exc : exceptions) {
        combined_rules += "\n!" + exc;
        combined_explanation += "；但保留 " + exc;
    }

    if (match_count > 0) {
        result.success = true;
        result.rules = combined_rules;
        result.explanation = combined_explanation;
        result.source = "template";
        if (g_logger) {
            g_logger->info("[NLFilter] 模板匹配成功: {} 条规则, 输入=\"{}\"", match_count, description);
        }
    }

    return result;
}

// ─── 内置模板表 ─────────────────────────────────────────────────

const std::vector<NLFilterGenerator::TemplateEntry>& NLFilterGenerator::get_templates() {
    static const std::vector<TemplateEntry> templates = {
        // --- 日志文件 ---
        {{"日志文件", "日志", "log文件", "log", ".log"},
         "*.log\n*.log.*",
         "忽略所有日志文件 (*.log, *.log.*)"},

        // --- 临时文件 ---
        {{"临时文件", "临时", "temp文件", "tmp文件", "temp", "tmp", "备份文件", "备份", "bak"},
         "*.tmp\n*.temp\n*.bak\n*.swp\n*~\n*.old",
         "忽略临时文件和备份文件 (*.tmp, *.temp, *.bak, *.swp, *~, *.old)"},

        // --- 编译/构建产物 ---
        {{"编译", "构建", "build", "编译产物", "构建产物", "编译输出", "构建输出", "编译结果"},
         "build/\ndist/\nout/\nbin/\nobj/\n*.o\n*.obj\n*.exe\n*.dll\n*.so\n*.dylib\n*.a\n*.lib",
         "忽略编译和构建产物目录及文件"},

        // --- 图片文件 ---
        {{"图片", "图像", "图片文件", "image", "图像文件", "照片"},
         "*.jpg\n*.jpeg\n*.png\n*.gif\n*.bmp\n*.webp\n*.svg\n*.ico\n*.tiff\n*.tif",
         "忽略常见图片格式文件"},

        // --- 视频文件 ---
        {{"视频", "视频文件", "video", "影片", "电影"},
         "*.mp4\n*.avi\n*.mkv\n*.mov\n*.wmv\n*.flv\n*.webm\n*.m4v",
         "忽略常见视频格式文件"},

        // --- 音频文件 ---
        {{"音频", "音频文件", "audio", "音乐", "声音"},
         "*.mp3\n*.wav\n*.flac\n*.aac\n*.ogg\n*.wma\n*.m4a",
         "忽略常见音频格式文件"},

        // --- 压缩文件 ---
        {{"压缩", "压缩文件", "archive", "压缩包", "归档"},
         "*.zip\n*.rar\n*.7z\n*.tar\n*.gz\n*.bz2\n*.xz\n*.tar.gz\n*.tar.bz2",
         "忽略压缩和归档文件"},

        // --- Node.js ---
        {{"node_modules", "npm", "yarn", "node依赖", "前端依赖", "nodejs"},
         "node_modules/\npackage-lock.json\nyarn.lock\n.npm/",
         "忽略 Node.js 依赖目录和锁文件"},

        // --- Python ---
        {{"python缓存", "pycache", "__pycache__", "pyc", "python编译", "虚拟环境", "venv"},
         "__pycache__/\n*.pyc\n*.pyo\n*.pyd\nvenv/\n.venv/\n*.egg-info/",
         "忽略 Python 缓存、编译文件和虚拟环境"},

        // --- Java ---
        {{"java编译", "class文件", "target目录", "maven", "gradle"},
         "target/\n*.class\n*.jar\n*.war\n.gradle/\nbuild/",
         "忽略 Java/Maven/Gradle 构建产物"},

        // --- IDE/编辑器 ---
        {{"ide配置", "编辑器配置", "ide", ".idea", ".vscode", "vs配置", "编辑器"},
         ".idea/\n.vscode/\n*.suo\n*.user\n*.sln.docstates\n.vs/\n*.swp\n*.swo\n*~",
         "忽略 IDE 和编辑器配置文件"},

        // --- macOS ---
        {{"macos", "ds_store", ".ds_store", "苹果系统"},
         ".DS_Store\n._*\n.Spotlight-V100\n.Trashes",
         "忽略 macOS 系统生成文件"},

        // --- Windows ---
        {{"windows系统", "thumbs.db", "desktop.ini", "windows缩略图"},
         "Thumbs.db\ndesktop.ini\nehthumbs.db\n$RECYCLE.BIN/",
         "忽略 Windows 系统生成文件"},

        // --- 文档 ---
        {{"文档", "doc文件", "文档文件", "word", "excel", "ppt", "办公文档"},
         "*.doc\n*.docx\n*.xls\n*.xlsx\n*.ppt\n*.pptx\n*.pdf",
         "忽略办公文档文件 (Word, Excel, PPT, PDF)"},

        // --- 大文件 ---
        {{"大文件", "二进制", "二进制文件"},
         "*.iso\n*.dmg\n*.img\n*.vmdk\n*.vdi\n*.bin",
         "忽略常见大型二进制文件 (ISO, 磁盘镜像等)"},

        // --- 数据库文件 ---
        {{"数据库", "数据库文件", "db文件", "sqlite"},
         "*.db\n*.sqlite\n*.sqlite3\n*.mdb",
         "忽略数据库文件"},

        // --- 缓存 ---
        {{"缓存", "cache", "缓存文件", "缓存目录"},
         ".cache/\n*.cache\ncache/",
         "忽略缓存目录和文件"},

        // --- 环境配置/密钥 ---
        {{"环境变量", "env文件", ".env", "密钥", "secret", "私钥", "证书"},
         ".env\n.env.*\n*.pem\n*.key\n*.crt\n*.p12\n*.pfx",
         "忽略环境变量文件和密钥证书文件"},

        // --- Git ---
        {{"git", ".git", "git目录", "gitignore"},
         ".git/\n.gitignore",
         "忽略 Git 版本控制目录"},

        // --- 所有隐藏文件 ---
        {{"隐藏文件", "点文件", "dotfile"},
         ".*",
         "忽略所有隐藏文件和目录（以 . 开头的文件）"},
    };

    return templates;
}

// ─── 文本工具函数 ───────────────────────────────────────────────

std::string NLFilterGenerator::normalize(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    bool last_was_space = false;
    for (unsigned char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            // ASCII 大写转小写
            result += static_cast<char>(ch + 32);
            last_was_space = false;
        } else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            // 合并多余空白
            if (!last_was_space && !result.empty()) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += static_cast<char>(ch);
            last_was_space = false;
        }
    }

    // 去除尾部空白
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    return result;
}

bool NLFilterGenerator::contains_keyword(const std::string& text, const std::string& keyword) {
    // 对于中文关键词（含多字节字符），直接做子串匹配
    // 对于纯 ASCII 关键词，做大小写不敏感匹配
    std::string lower_keyword = normalize(keyword);

    // 子串搜索
    return text.find(lower_keyword) != std::string::npos;
}

std::vector<std::string> NLFilterGenerator::extract_exceptions(const std::string& text) {
    std::vector<std::string> exceptions;
    
    // 匹配常见的取反模式：
    //   "但保留 xxx"、"但是保留 xxx"、"除了 xxx"、"排除 xxx 以外"
    //   "不包括 xxx"、"xxx 除外"
    // 这些模式中的 xxx 通常是文件名或通配符模式
    
    // 简化版：提取 "但保留"/"除了"/"不包括" 后跟的文件名模式
    static const std::vector<std::string> exception_prefixes = {
        "但保留", "但是保留", "除了", "不包括", "不要忽略", "保留",
        "except", "but keep", "exclude"
    };

    std::string normalized = normalize(text);

    for (const auto& prefix : exception_prefixes) {
        auto pos = normalized.find(prefix);
        if (pos == std::string::npos) continue;

        // 提取 prefix 后面的内容
        std::string after = normalized.substr(pos + prefix.size());
        
        // 去除前导空白
        size_t start = 0;
        while (start < after.size() && after[start] == ' ') ++start;
        after = after.substr(start);

        if (after.empty()) continue;

        // 提取文件名模式（到下一个中文标点或句尾）
        std::string pattern;
        for (size_t i = 0; i < after.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(after[i]);
            // 遇到中文标点或常见分隔符时停止
            if (ch == ',' || ch == ';' || ch == '\n') break;
            // 检测 UTF-8 中文标点（简化：遇到某些终止模式停止）
            if (i + 2 < after.size() && ch == 0xEF) break;  // 全角标点起始字节
            pattern += after[i];
        }

        // 去除尾部空白
        while (!pattern.empty() && pattern.back() == ' ') {
            pattern.pop_back();
        }

        if (!pattern.empty()) {
            // 如果用户说的是纯文件名（没有通配符），原样保留
            // 用户可能说 "error.log" 或 "*.important"
            exceptions.push_back(pattern);
        }
    }

    return exceptions;
}

// ═══════════════════════════════════════════════════════════════
// 第二层：LLM API 调用
// ═══════════════════════════════════════════════════════════════

NLFilterGenerator::Result NLFilterGenerator::generate_from_llm(
    const std::string& description, const std::string& existing_rules) const {
    
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
        std::string prompt = build_llm_prompt(description, existing_rules);
        nlohmann::json request_body = {
            {"model", config.model.empty() ? "deepseek-chat" : config.model},
            {"messages", {{
                {"role", "user"},
                {"content", prompt}
            }}},
            {"temperature", 0.3},
            {"max_tokens", 1024}
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

        // 提取规则
        std::string rules = extract_rules_from_response(content);
        if (rules.empty()) {
            result.success = false;
            result.error = "LLM 未生成有效的过滤规则";
            return result;
        }

        // 验证规则合法性
        if (!validate_rules(rules)) {
            result.success = false;
            result.error = "LLM 生成的规则格式无效";
            return result;
        }

        result.success = true;
        result.rules = rules;
        result.explanation = "由 AI 根据描述自动生成";
        result.source = "llm";

        if (g_logger) {
            g_logger->info("[NLFilter] LLM 生成成功: 输入=\"{}\", 规则行数={}", 
                description, std::count(rules.begin(), rules.end(), '\n') + 1);
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("LLM API 调用异常: ") + e.what();
        if (g_logger) g_logger->error("[NLFilter] LLM 异常: {}", e.what());
    }

    return result;
}

std::string NLFilterGenerator::build_llm_prompt(
    const std::string& description, const std::string& existing_rules) {
    
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

## 要求：
1. 只输出过滤规则，每行一条
2. 可以添加 # 注释来解释每组规则的用途
3. 不要输出其他任何解释性文字
4. 把规则放在 ```rules ... ``` 代码块中
5. 不要重复已有的规则

)";

    if (!existing_rules.empty()) {
        prompt += "## 用户已有的规则：\n```\n" + existing_rules + "\n```\n\n";
    }

    prompt += "## 用户需求：\n" + description + "\n\n请生成对应的过滤规则：";

    return prompt;
}

std::string NLFilterGenerator::extract_rules_from_response(const std::string& response) {
    // 尝试从 ```rules ... ``` 或 ``` ... ``` 代码块中提取
    std::string result;
    
    // 查找代码块
    size_t block_start = std::string::npos;
    size_t block_end = std::string::npos;

    // 尝试匹配 ```rules 或 ```gitignore 或 ``` 
    std::vector<std::string> markers = {"```rules", "```gitignore", "```"};
    for (const auto& marker : markers) {
        auto pos = response.find(marker);
        if (pos != std::string::npos) {
            // 跳过该行（到换行符后）
            block_start = response.find('\n', pos);
            if (block_start != std::string::npos) {
                ++block_start;
                // 找到结束的 ```
                block_end = response.find("```", block_start);
                if (block_end != std::string::npos) {
                    result = response.substr(block_start, block_end - block_start);
                    break;
                }
            }
        }
    }

    // 如果没有代码块，尝试直接使用整个响应
    if (result.empty()) {
        result = response;
    }

    // 清理：去除空行首尾
    while (!result.empty() && (result.front() == '\n' || result.front() == '\r')) {
        result.erase(result.begin());
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

    return result;
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

}  // namespace VeritasSync
