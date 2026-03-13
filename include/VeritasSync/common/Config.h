#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <sstream>

namespace VeritasSync {

enum class SyncMode {
    OneWay,        // 单向：仅 Source -> Destination
    BiDirectional  // 双向：Source <-> Destination
};

enum class SyncRole { Source, Destination };

// 单个同步任务的结构
struct SyncTask {
    std::string sync_key;
    std::string role;  // "source" or "destination" (在双向模式下主要决定谁先发起连接，或作为逻辑标识)
    std::string sync_folder;
    SyncMode mode = SyncMode::OneWay;
};

// 顶级配置结构
struct Config {
    // --- 设备唯一标识符（首次启动时自动生成） ---
    std::string device_id;
    // ----------------------------
    
    std::string tracker_host = "47.121.187.240";
    unsigned short tracker_port = 9988;

    std::string stun_host = "stun.l.google.com";
    unsigned short stun_port = 19302;

    std::string turn_host;
    unsigned short turn_port = 3478;
    std::string turn_username;
    std::string turn_password;
    // ----------------------------

    // --- 日志级别配置  ---
    std::string log_level = "info";  // debug, info, warn, error
    std::string libjuice_log_level = "info";
    // ----------------------------

    // --- 性能参数配置  ---
    uint32_t kcp_update_interval_ms = 20;  // KCP更新间隔 (10-100ms)
    size_t chunk_size = 16384;  // 文件分块大小 (bytes)
    uint32_t kcp_window_size = 256;  // KCP窗口大小
    uint32_t file_hash_retry_delay_ms = 250;  // 文件哈希重试延迟
    // ----------------------------

    // --- WebUI 配置  ---
    unsigned short webui_port = 8800;  // Web 控制台端口
    // ----------------------------

    // --- LLM API 配置（用于自然语言生成过滤规则，可选）---
    std::string llm_api_url;                    // API 端点（如 https://api.deepseek.com）
    std::string llm_api_key;                    // API 密钥
    std::string llm_model = "deepseek-chat";    // 模型名称
    // ----------------------------

    std::vector<SyncTask> tasks;
};

// --- nlohmann/json 集成 ---
NLOHMANN_JSON_SERIALIZE_ENUM(SyncMode, {{SyncMode::OneWay, "oneway"}, {SyncMode::BiDirectional, "bidirectional"}})

inline void to_json(nlohmann::json& j, const SyncTask& task) {
    j = nlohmann::json{
        {"sync_key", task.sync_key}, {"role", task.role}, {"sync_folder", task.sync_folder}, {"mode", task.mode}};
}

inline void from_json(const nlohmann::json& j, SyncTask& task) {
    j.at("sync_key").get_to(task.sync_key);
    j.at("role").get_to(task.role);
    j.at("sync_folder").get_to(task.sync_folder);
    if (j.contains("mode")) {
        j.at("mode").get_to(task.mode);
    } else {
        task.mode = SyncMode::OneWay;
    }
}

inline void to_json(nlohmann::json& j, const Config& config) {
    j = nlohmann::json{{"device_id", config.device_id},
                       {"tracker_host", config.tracker_host},
                       {"tracker_port", config.tracker_port},
                       {"stun_host", config.stun_host},
                       {"stun_port", config.stun_port},
                       {"turn_host", config.turn_host},
                       {"turn_port", config.turn_port},
                       {"turn_username", config.turn_username},
                       {"turn_password", config.turn_password},
                       {"log_level", config.log_level},
                       {"libjuice_log_level", config.libjuice_log_level},
                       {"kcp_update_interval_ms", config.kcp_update_interval_ms},
                       {"chunk_size", config.chunk_size},
                       {"kcp_window_size", config.kcp_window_size},
                       {"file_hash_retry_delay_ms", config.file_hash_retry_delay_ms},
                       {"webui_port", config.webui_port},
                       {"llm_api_url", config.llm_api_url},
                       {"llm_api_key", config.llm_api_key},
                       {"llm_model", config.llm_model},
                       {"tasks", config.tasks}};
}

inline void from_json(const nlohmann::json& j, Config& config) {
    // --- 加载 device_id (如果存在) ---
    if (j.contains("device_id")) j.at("device_id").get_to(config.device_id);
    // ---------------------------------
    
    j.at("tracker_host").get_to(config.tracker_host);
    j.at("tracker_port").get_to(config.tracker_port);

    // --- 加载 STUN (如果存在) ---
    if (j.contains("stun_host")) j.at("stun_host").get_to(config.stun_host);
    if (j.contains("stun_port")) j.at("stun_port").get_to(config.stun_port);
    // ---------------------------------

    // --- 加载 TURN (如果存在) ---
    if (j.contains("turn_host")) j.at("turn_host").get_to(config.turn_host);
    if (j.contains("turn_port")) j.at("turn_port").get_to(config.turn_port);
    if (j.contains("turn_username")) j.at("turn_username").get_to(config.turn_username);
    if (j.contains("turn_password")) j.at("turn_password").get_to(config.turn_password);
    // ---------------------------------

    // --- 加载日志和性能配置 (如果存在) ---
    if (j.contains("log_level")) j.at("log_level").get_to(config.log_level);
    if (j.contains("libjuice_log_level")) j.at("libjuice_log_level").get_to(config.libjuice_log_level);
    if (j.contains("kcp_update_interval_ms")) j.at("kcp_update_interval_ms").get_to(config.kcp_update_interval_ms);
    if (j.contains("chunk_size")) j.at("chunk_size").get_to(config.chunk_size);
    if (j.contains("kcp_window_size")) j.at("kcp_window_size").get_to(config.kcp_window_size);
    if (j.contains("file_hash_retry_delay_ms")) j.at("file_hash_retry_delay_ms").get_to(config.file_hash_retry_delay_ms);
    // ---------------------------------

    // --- 加载 WebUI 配置 (如果存在) ---
    if (j.contains("webui_port")) j.at("webui_port").get_to(config.webui_port);
    // ---------------------------------

    // --- 加载 LLM API 配置 (如果存在) ---
    if (j.contains("llm_api_url")) j.at("llm_api_url").get_to(config.llm_api_url);
    if (j.contains("llm_api_key")) j.at("llm_api_key").get_to(config.llm_api_key);
    if (j.contains("llm_model")) j.at("llm_model").get_to(config.llm_model);
    // ---------------------------------

    j.at("tasks").get_to(config.tasks);
}

/// 配置验证：检查所有字段的有效性，返回错误信息列表（空 = 全部通过）
std::vector<std::string> validate_config(const Config& config);

// 辅助函数：生成 UUID v4
std::string generate_uuid_v4();

// 辅助函数：加载配置或创建默认配置
Config load_config_or_create_default(const std::string& config_path = "config.json");

}  // namespace VeritasSync
