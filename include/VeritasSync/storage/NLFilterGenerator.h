#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace VeritasSync {

/// 自然语言 → .veritasignore 规则生成器
/// 
/// 两层架构：
///   第一层：本地模板匹配引擎（覆盖 80% 常见场景，零延迟）
///   第二层：LLM API 辅助生成（处理复杂/模糊意图，需配置 API）
class NLFilterGenerator {
public:
    /// 生成结果
    struct Result {
        bool success = false;
        std::string rules;          // 生成的 .veritasignore 规则文本（多行）
        std::string explanation;    // 对生成规则的解释说明
        std::string source;         // 生成来源："template" 或 "llm"
        std::string error;          // 错误信息（失败时）
    };

    /// LLM API 配置
    struct LLMConfig {
        std::string api_url;        // API 端点 URL（如 https://api.deepseek.com）
        std::string api_key;        // API 密钥
        std::string model;          // 模型名称（如 deepseek-chat）
        int timeout_seconds = 30;   // 请求超时时间
    };

    NLFilterGenerator() = default;

    /// 设置 LLM 配置（可选，不设置则仅使用模板引擎）
    void set_llm_config(const LLMConfig& config);

    /// 检查 LLM 是否已配置
    bool has_llm() const;

    /// 核心接口：将自然语言描述转换为过滤规则
    /// @param description 用户的自然语言描述
    /// @param existing_rules 现有的规则内容（用于避免重复）
    /// @return 生成结果
    Result generate(const std::string& description, const std::string& existing_rules = "") const;

    /// 仅使用模板引擎生成（不调用 LLM）
    Result generate_from_template(const std::string& description) const;

    /// 仅使用 LLM 生成
    Result generate_from_llm(const std::string& description, const std::string& existing_rules = "") const;

private:
    // ─── 模板引擎内部实现 ───────────────────────────────────────

    /// 单条模板映射规则
    struct TemplateEntry {
        std::vector<std::string> keywords;  // 触发关键词列表（任一匹配即触发）
        std::string rules;                  // 对应的 .veritasignore 规则
        std::string explanation;            // 对规则的说明
    };

    /// 获取内置模板表（静态，只初始化一次）
    static const std::vector<TemplateEntry>& get_templates();

    /// 文本预处理：转小写、去除多余空白
    static std::string normalize(const std::string& text);

    /// 检查文本中是否包含指定关键词
    static bool contains_keyword(const std::string& text, const std::string& keyword);

    /// 解析取反意图（如 "但保留 xxx"、"除了 xxx"）
    static std::vector<std::string> extract_exceptions(const std::string& text);

    // ─── LLM 相关 ──────────────────────────────────────────────

    /// 构建发送给 LLM 的提示词
    static std::string build_llm_prompt(const std::string& description, const std::string& existing_rules);

    /// 从 LLM 响应中提取规则文本
    static std::string extract_rules_from_response(const std::string& response);

    /// 验证生成的规则是否合法（能被 glob_to_regex 编译）
    static bool validate_rules(const std::string& rules);

    mutable std::mutex m_mutex;
    LLMConfig m_llm_config;
    bool m_has_llm = false;
};

}  // namespace VeritasSync
