#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace VeritasSync {

/// 自然语言 → .veritasignore 规则生成器
///
/// 通过 LLM API 将用户的自然语言描述转换为 .gitignore 兼容的过滤规则。
/// 需要在 config.json 中配置 llm_api_key 才能使用。
class NLFilterGenerator {
public:
    /// 生成结果
    struct Result {
        bool success = false;
        std::string rules;          // 生成的 .veritasignore 规则文本（多行）
        std::string explanation;    // 对生成规则的解释说明
        std::string source;         // 生成来源："llm"
        std::string error;          // 错误信息（失败时）
        size_t affected_files = 0;  // Dry-Run：规则将影响的文件数量
    };

    /// LLM API 配置
    struct LLMConfig {
        std::string api_url;        // API 端点 URL（如 https://dashscope.aliyuncs.com/compatible-mode）
        std::string api_key;        // API 密钥
        std::string model;          // 模型名称（如 qwen-turbo）
        int timeout_seconds = 30;   // 请求超时时间
        double temperature = 0.3;   // LLM 温度参数
        int max_tokens = 1024;      // LLM 最大输出 token 数
        int max_scan_files = 50000; // 目录扫描最大文件数
        int large_dir_threshold = 500; // 大目录折叠阈值
    };

    NLFilterGenerator() = default;

    /// 设置 LLM 配置
    void set_llm_config(const LLMConfig& config);

    /// 检查 LLM 是否已配置
    bool has_llm() const;

    /// 核心接口：将自然语言描述转换为过滤规则
    /// @param description 用户的自然语言描述
    /// @param existing_rules 现有的规则内容（用于避免重复）
    /// @param sync_folder 同步目录路径（可选，提供后会扫描目录结构辅助生成更精准的规则）
    /// @return 生成结果
    Result generate(const std::string& description,
                    const std::string& existing_rules = "",
                    const std::string& sync_folder = "") const;

private:
    // ─── LLM 相关 ──────────────────────────────────────────────

    /// 调用 LLM 生成规则
    Result generate_from_llm(const std::string& description,
                             const std::string& existing_rules,
                             const std::string& dir_summary) const;

    /// 构建发送给 LLM 的提示词
    static std::string build_llm_prompt(const std::string& description,
                                        const std::string& existing_rules,
                                        const std::string& dir_summary);

    /// 扫描目录生成摘要（按目录聚合文件扩展名和数量）
    static std::string build_directory_summary(const std::string& root_path, size_t max_chars = 3000,
                                               size_t max_scan_files = 50000, size_t large_dir_threshold = 500);

    /// 从用户描述中提取关键词，在文件树中搜索匹配的样本路径
    static std::string search_relevant_samples(const std::string& root_path,
                                               const std::string& description,
                                               size_t max_samples = 10,
                                               size_t max_scan_files = 50000);

    /// Dry-Run：用生成的规则在文件树上试跑，返回将被忽略的文件数量
    static size_t dry_run_rules(const std::string& root_path, const std::string& rules,
                                size_t max_scan_files = 50000);

    /// 从 LLM 响应中提取规则（JSON 大括号提取法）
    struct ParsedResponse {
        std::string rules;          // 规则文本（多行）
        std::string explanation;    // LLM 给出的说明
    };
    static ParsedResponse extract_rules_from_response(const std::string& response);

    /// 验证生成的规则是否合法
    static bool validate_rules(const std::string& rules);

    mutable std::mutex m_mutex;
    LLMConfig m_llm_config;
    bool m_has_llm = false;
};

}  // namespace VeritasSync
