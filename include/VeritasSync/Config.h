#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace VeritasSync {

static const std::vector<std::pair<std::string, unsigned short>> kBackupStunServers = {
    {"stun.l.google.com", 19302},
    {"47.121.187.240", 3478},
};

enum class SyncMode {
    OneWay,        // 单向：仅 Source -> Destination
    BiDirectional  // 双向：Source <-> Destination
};

// 单个同步任务的结构
struct SyncTask {
    std::string sync_key;
    std::string role;  // "source" or "destination" (在双向模式下主要决定谁先发起连接，或作为逻辑标识)
    std::string sync_folder;
    SyncMode mode = SyncMode::OneWay;
};

// 顶级配置结构
struct Config {
    std::string tracker_host = "47.121.187.240";
    unsigned short tracker_port = 9988;

    std::string stun_host = "stun.l.google.com";
    unsigned short stun_port = 19302;

    std::string stun_list_url = "https://raw.githubusercontent.com/pradt2/always-online-stun/master/valid_hosts.txt";
    bool enable_multi_stun_probing = true;  // 开关

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
    j = nlohmann::json{{"tracker_host", config.tracker_host},
                       {"tracker_port", config.tracker_port},
                       {"stun_host", config.stun_host},
                       {"stun_port", config.stun_port},
                       {"turn_host", config.turn_host},
                       {"turn_port", config.turn_port},
                       {"turn_username", config.turn_username},
                       {"turn_password", config.turn_password},
                       {"stun_list_url", config.stun_list_url},
                       {"enable_multi_stun_probing", config.enable_multi_stun_probing},
                       {"log_level", config.log_level},
                       {"libjuice_log_level", config.libjuice_log_level},
                       {"kcp_update_interval_ms", config.kcp_update_interval_ms},
                       {"chunk_size", config.chunk_size},
                       {"kcp_window_size", config.kcp_window_size},
                       {"file_hash_retry_delay_ms", config.file_hash_retry_delay_ms},
                       {"tasks", config.tasks}};
}

inline void from_json(const nlohmann::json& j, Config& config) {
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

    if (j.contains("stun_list_url")) j.at("stun_list_url").get_to(config.stun_list_url);
    if (j.contains("enable_multi_stun_probing"))
        j.at("enable_multi_stun_probing").get_to(config.enable_multi_stun_probing);

    j.at("tasks").get_to(config.tasks);
}

// 辅助函数：加载配置或创建默认配置
inline Config load_config_or_create_default(const std::string& config_path = "config.json") {
    std::ifstream f(config_path);
    if (f.good()) {
        nlohmann::json j;
        f >> j;
        // 增加容错：如果配置文件里缺少某些字段，由 from_json 处理或使用结构体默认值
        return j.get<Config>();
    } else {
        Config defaultConfig;

        // TURN 默认留空，防止连接无效地址导致延迟
        defaultConfig.turn_host = "";
        defaultConfig.turn_port = 3478;
        defaultConfig.turn_username = "";
        defaultConfig.turn_password = "";

        // STUN 默认使用 Google 的，比较稳定
        defaultConfig.stun_host = "stun.l.google.com";
        defaultConfig.stun_port = 19302;

        defaultConfig.tasks = {};

        std::ofstream o(config_path);
        o << nlohmann::json(defaultConfig).dump(4) << std::endl;
        return defaultConfig;
    }
}

}  // namespace VeritasSync
