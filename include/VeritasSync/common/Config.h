#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cctype>
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
    std::string role;  // "source" or "destination"
    std::string sync_folder;
    SyncMode mode = SyncMode::OneWay;
};

// ═══════════════════════════════════════════════════════════════
// 顶级配置结构（分组嵌套式）
// ═══════════════════════════════════════════════════════════════

struct Config {
    // --- 设备唯一标识符（首次启动时自动生成） ---
    std::string device_id;

    // --- 网络配置 ---
    struct Network {
        std::string tracker_host = "47.121.187.240";
        unsigned short tracker_port = 9988;
        int tracker_reconnect_interval_seconds = 5;
        int tracker_max_packet_size_mb = 1;

        std::string stun_host = "stun.l.google.com";
        unsigned short stun_port = 19302;

        std::string turn_host;
        unsigned short turn_port = 3478;
        std::string turn_username;
        std::string turn_password;

        int ice_answer_wait_timeout_seconds = 30;
        int upnp_discover_timeout_ms = 2000;
        int peer_cleanup_interval_minutes = 5;
    } network;

    // --- 传输配置 ---
    struct Transfer {
        size_t chunk_size = 16384;
        int stall_threshold_ms = 5000;
        int zombie_threshold_seconds = 60;
        int receive_timeout_minutes = 10;
        int congestion_wait_high_ms = 200;
        int congestion_wait_low_ms = 100;
        int congestion_high_multiplier = 4;
        int congestion_threshold = 256;
        double speed_update_interval_sec = 0.5;
        int file_open_max_retries = 5;
        int file_open_retry_delay_ms = 200;
        size_t max_total_chunks = 8388608;
        size_t max_path_length = 4096;
        int broadcast_file_update_batch_size = 50;
        int broadcast_file_delete_batch_size = 100;
    } transfer;

    // --- KCP 配置 ---
    struct Kcp {
        uint32_t update_interval_ms = 20;
        uint32_t window_size = 256;
    } kcp;

    // --- 同步逻辑配置 ---
    struct Sync {
        int session_timeout_seconds = 60;
        int flow_control_threshold = 1024;
        int flow_control_sleep_ms = 20;
        int file_change_debounce_delay_ms = 5000;
        int batch_trigger_threshold = 100;
        int batch_min_interval_ms = 1000;
        int file_hash_retry_delay_ms = 250;
    } sync;

    // --- 日志配置 ---
    struct Logging {
        std::string level = "info";
        std::string libjuice_level = "info";
        int max_file_size_mb = 5;
        int max_files = 3;
        int thread_pool_size = 8192;
    } logging;

    // --- WebUI 配置 ---
    struct WebUI {
        unsigned short port = 8800;
        int log_tail_status_bytes = 16384;
        int log_tail_task_bytes = 32768;
        int auth_token_bytes = 32;
        int max_description_length = 4096;
        int max_rules_length = 10000;
    } webui;

    // --- LLM API 配置 ---
    struct LLM {
        std::string api_url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
        std::string api_key;
        std::string model = "qwen-turbo";
        double temperature = 0.3;
        int max_tokens = 1024;
        int timeout_seconds = 30;
        int max_scan_files = 50000;
        int large_dir_threshold = 500;
    } llm;

    // --- 数据库配置 ---
    struct Database {
        int busy_timeout_ms = 5000;
    } database;

    // --- 高级配置 ---
    struct Advanced {
        int worker_thread_count = 0;     // 0 = auto (hardware_concurrency)
        int hash_thread_count = 0;       // 0 = auto
        int hash_read_buffer_size_kb = 64;
        int tracker_max_message_length_bytes = 262144;
    } advanced;

    // --- 同步任务列表 ---
    std::vector<SyncTask> tasks;
};

// ═══════════════════════════════════════════════════════════════
// nlohmann/json 集成
// ═══════════════════════════════════════════════════════════════

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

// --- to_json / from_json：拆分为 config.json（用户配置）和 advanced.json（专家调优）---

// 辅助宏
#define LOAD_OPT(src, key, dst) do { if ((src).contains(key)) (src).at(key).get_to(dst); } while(0)

/// config.json 序列化（用户常改的配置）
inline nlohmann::json config_to_json(const Config& c) {
    nlohmann::json j = {
        {"device_id", c.device_id},
        {"network", {
            {"tracker_host", c.network.tracker_host},
            {"tracker_port", c.network.tracker_port},
            {"stun_host", c.network.stun_host},
            {"stun_port", c.network.stun_port},
            {"turn_host", c.network.turn_host},
            {"turn_port", c.network.turn_port},
            {"turn_username", c.network.turn_username},
            {"turn_password", c.network.turn_password}
        }},
        {"logging", {
            {"level", c.logging.level}
        }},
        {"webui", {
            {"port", c.webui.port}
        }},
        {"llm", {
            {"api_url", c.llm.api_url},
            {"api_key", c.llm.api_key},
            {"model", c.llm.model}
        }},
        {"tasks", c.tasks}
    };
    return j;
}

/// config.json 反序列化
inline void config_from_json(const nlohmann::json& j, Config& c) {
    LOAD_OPT(j, "device_id", c.device_id);

    if (j.contains("network")) {
        const auto& n = j["network"];
        LOAD_OPT(n, "tracker_host", c.network.tracker_host);
        LOAD_OPT(n, "tracker_port", c.network.tracker_port);
        LOAD_OPT(n, "stun_host", c.network.stun_host);
        LOAD_OPT(n, "stun_port", c.network.stun_port);
        LOAD_OPT(n, "turn_host", c.network.turn_host);
        LOAD_OPT(n, "turn_port", c.network.turn_port);
        LOAD_OPT(n, "turn_username", c.network.turn_username);
        LOAD_OPT(n, "turn_password", c.network.turn_password);
    }
    if (j.contains("logging")) {
        const auto& l = j["logging"];
        LOAD_OPT(l, "level", c.logging.level);
    }
    if (j.contains("webui")) {
        const auto& w = j["webui"];
        LOAD_OPT(w, "port", c.webui.port);
    }
    if (j.contains("llm")) {
        const auto& l = j["llm"];
        LOAD_OPT(l, "api_url", c.llm.api_url);
        LOAD_OPT(l, "api_key", c.llm.api_key);
        LOAD_OPT(l, "model", c.llm.model);
    }
    if (j.contains("tasks")) {
        j.at("tasks").get_to(c.tasks);
    }
}

/// advanced.json 序列化（专家调优配置）
inline nlohmann::json advanced_to_json(const Config& c) {
    return {
        {"network", {
            {"tracker_reconnect_interval_seconds", c.network.tracker_reconnect_interval_seconds},
            {"tracker_max_packet_size_mb", c.network.tracker_max_packet_size_mb},
            {"ice_answer_wait_timeout_seconds", c.network.ice_answer_wait_timeout_seconds},
            {"upnp_discover_timeout_ms", c.network.upnp_discover_timeout_ms},
            {"peer_cleanup_interval_minutes", c.network.peer_cleanup_interval_minutes}
        }},
        {"transfer", {
            {"chunk_size", c.transfer.chunk_size},
            {"stall_threshold_ms", c.transfer.stall_threshold_ms},
            {"zombie_threshold_seconds", c.transfer.zombie_threshold_seconds},
            {"receive_timeout_minutes", c.transfer.receive_timeout_minutes},
            {"congestion_wait_high_ms", c.transfer.congestion_wait_high_ms},
            {"congestion_wait_low_ms", c.transfer.congestion_wait_low_ms},
            {"congestion_high_multiplier", c.transfer.congestion_high_multiplier},
            {"congestion_threshold", c.transfer.congestion_threshold},
            {"speed_update_interval_sec", c.transfer.speed_update_interval_sec},
            {"file_open_max_retries", c.transfer.file_open_max_retries},
            {"file_open_retry_delay_ms", c.transfer.file_open_retry_delay_ms},
            {"max_total_chunks", c.transfer.max_total_chunks},
            {"max_path_length", c.transfer.max_path_length},
            {"broadcast_file_update_batch_size", c.transfer.broadcast_file_update_batch_size},
            {"broadcast_file_delete_batch_size", c.transfer.broadcast_file_delete_batch_size}
        }},
        {"kcp", {
            {"update_interval_ms", c.kcp.update_interval_ms},
            {"window_size", c.kcp.window_size}
        }},
        {"sync", {
            {"session_timeout_seconds", c.sync.session_timeout_seconds},
            {"flow_control_threshold", c.sync.flow_control_threshold},
            {"flow_control_sleep_ms", c.sync.flow_control_sleep_ms},
            {"file_change_debounce_delay_ms", c.sync.file_change_debounce_delay_ms},
            {"batch_trigger_threshold", c.sync.batch_trigger_threshold},
            {"batch_min_interval_ms", c.sync.batch_min_interval_ms},
            {"file_hash_retry_delay_ms", c.sync.file_hash_retry_delay_ms}
        }},
        {"logging", {
            {"libjuice_level", c.logging.libjuice_level},
            {"max_file_size_mb", c.logging.max_file_size_mb},
            {"max_files", c.logging.max_files},
            {"thread_pool_size", c.logging.thread_pool_size}
        }},
        {"webui", {
            {"log_tail_status_bytes", c.webui.log_tail_status_bytes},
            {"log_tail_task_bytes", c.webui.log_tail_task_bytes},
            {"auth_token_bytes", c.webui.auth_token_bytes},
            {"max_description_length", c.webui.max_description_length},
            {"max_rules_length", c.webui.max_rules_length}
        }},
        {"llm", {
            {"temperature", c.llm.temperature},
            {"max_tokens", c.llm.max_tokens},
            {"timeout_seconds", c.llm.timeout_seconds},
            {"max_scan_files", c.llm.max_scan_files},
            {"large_dir_threshold", c.llm.large_dir_threshold}
        }},
        {"database", {
            {"busy_timeout_ms", c.database.busy_timeout_ms}
        }},
        {"advanced", {
            {"worker_thread_count", c.advanced.worker_thread_count},
            {"hash_thread_count", c.advanced.hash_thread_count},
            {"hash_read_buffer_size_kb", c.advanced.hash_read_buffer_size_kb},
            {"tracker_max_message_length_bytes", c.advanced.tracker_max_message_length_bytes}
        }}
    };
}

/// advanced.json 反序列化
inline void advanced_from_json(const nlohmann::json& j, Config& c) {
    if (j.contains("network")) {
        const auto& n = j["network"];
        LOAD_OPT(n, "tracker_reconnect_interval_seconds", c.network.tracker_reconnect_interval_seconds);
        LOAD_OPT(n, "tracker_max_packet_size_mb", c.network.tracker_max_packet_size_mb);
        LOAD_OPT(n, "ice_answer_wait_timeout_seconds", c.network.ice_answer_wait_timeout_seconds);
        LOAD_OPT(n, "upnp_discover_timeout_ms", c.network.upnp_discover_timeout_ms);
        LOAD_OPT(n, "peer_cleanup_interval_minutes", c.network.peer_cleanup_interval_minutes);
    }
    if (j.contains("transfer")) {
        const auto& t = j["transfer"];
        LOAD_OPT(t, "chunk_size", c.transfer.chunk_size);
        LOAD_OPT(t, "stall_threshold_ms", c.transfer.stall_threshold_ms);
        LOAD_OPT(t, "zombie_threshold_seconds", c.transfer.zombie_threshold_seconds);
        LOAD_OPT(t, "receive_timeout_minutes", c.transfer.receive_timeout_minutes);
        LOAD_OPT(t, "congestion_wait_high_ms", c.transfer.congestion_wait_high_ms);
        LOAD_OPT(t, "congestion_wait_low_ms", c.transfer.congestion_wait_low_ms);
        LOAD_OPT(t, "congestion_high_multiplier", c.transfer.congestion_high_multiplier);
        LOAD_OPT(t, "congestion_threshold", c.transfer.congestion_threshold);
        LOAD_OPT(t, "speed_update_interval_sec", c.transfer.speed_update_interval_sec);
        LOAD_OPT(t, "file_open_max_retries", c.transfer.file_open_max_retries);
        LOAD_OPT(t, "file_open_retry_delay_ms", c.transfer.file_open_retry_delay_ms);
        LOAD_OPT(t, "max_total_chunks", c.transfer.max_total_chunks);
        LOAD_OPT(t, "max_path_length", c.transfer.max_path_length);
        LOAD_OPT(t, "broadcast_file_update_batch_size", c.transfer.broadcast_file_update_batch_size);
        LOAD_OPT(t, "broadcast_file_delete_batch_size", c.transfer.broadcast_file_delete_batch_size);
    }
    if (j.contains("kcp")) {
        const auto& k = j["kcp"];
        LOAD_OPT(k, "update_interval_ms", c.kcp.update_interval_ms);
        LOAD_OPT(k, "window_size", c.kcp.window_size);
    }
    if (j.contains("sync")) {
        const auto& s = j["sync"];
        LOAD_OPT(s, "session_timeout_seconds", c.sync.session_timeout_seconds);
        LOAD_OPT(s, "flow_control_threshold", c.sync.flow_control_threshold);
        LOAD_OPT(s, "flow_control_sleep_ms", c.sync.flow_control_sleep_ms);
        LOAD_OPT(s, "file_change_debounce_delay_ms", c.sync.file_change_debounce_delay_ms);
        LOAD_OPT(s, "batch_trigger_threshold", c.sync.batch_trigger_threshold);
        LOAD_OPT(s, "batch_min_interval_ms", c.sync.batch_min_interval_ms);
        LOAD_OPT(s, "file_hash_retry_delay_ms", c.sync.file_hash_retry_delay_ms);
    }
    if (j.contains("logging")) {
        const auto& l = j["logging"];
        LOAD_OPT(l, "libjuice_level", c.logging.libjuice_level);
        LOAD_OPT(l, "max_file_size_mb", c.logging.max_file_size_mb);
        LOAD_OPT(l, "max_files", c.logging.max_files);
        LOAD_OPT(l, "thread_pool_size", c.logging.thread_pool_size);
    }
    if (j.contains("webui")) {
        const auto& w = j["webui"];
        LOAD_OPT(w, "log_tail_status_bytes", c.webui.log_tail_status_bytes);
        LOAD_OPT(w, "log_tail_task_bytes", c.webui.log_tail_task_bytes);
        LOAD_OPT(w, "auth_token_bytes", c.webui.auth_token_bytes);
        LOAD_OPT(w, "max_description_length", c.webui.max_description_length);
        LOAD_OPT(w, "max_rules_length", c.webui.max_rules_length);
    }
    if (j.contains("llm")) {
        const auto& l = j["llm"];
        LOAD_OPT(l, "temperature", c.llm.temperature);
        LOAD_OPT(l, "max_tokens", c.llm.max_tokens);
        LOAD_OPT(l, "timeout_seconds", c.llm.timeout_seconds);
        LOAD_OPT(l, "max_scan_files", c.llm.max_scan_files);
        LOAD_OPT(l, "large_dir_threshold", c.llm.large_dir_threshold);
    }
    if (j.contains("database")) {
        const auto& d = j["database"];
        LOAD_OPT(d, "busy_timeout_ms", c.database.busy_timeout_ms);
    }
    if (j.contains("advanced")) {
        const auto& a = j["advanced"];
        LOAD_OPT(a, "worker_thread_count", c.advanced.worker_thread_count);
        LOAD_OPT(a, "hash_thread_count", c.advanced.hash_thread_count);
        LOAD_OPT(a, "hash_read_buffer_size_kb", c.advanced.hash_read_buffer_size_kb);
        LOAD_OPT(a, "tracker_max_message_length_bytes", c.advanced.tracker_max_message_length_bytes);
    }
}

/// nlohmann to_json/from_json 适配（合并两个文件的所有字段）
inline void to_json(nlohmann::json& j, const Config& c) {
    // 合并 config 和 advanced 的所有字段到一个 JSON（用于 WebUI API 等需要完整配置的场景）
    j = config_to_json(c);
    auto adv = advanced_to_json(c);
    // 逐组合并（advanced 的字段补充到 config 的同名组中）
    for (auto& [key, val] : adv.items()) {
        if (j.contains(key) && j[key].is_object() && val.is_object()) {
            j[key].update(val);
        } else {
            j[key] = val;
        }
    }
}

inline void from_json(const nlohmann::json& j, Config& c) {
    config_from_json(j, c);
    advanced_from_json(j, c);
}

#undef LOAD_OPT

// ═══════════════════════════════════════════════════════════════
// 工具函数
// ═══════════════════════════════════════════════════════════════

/// 端口验证辅助函数（unsigned short 版本，只需检查 > 0）
inline bool is_valid_port(unsigned short port) { return port > 0; }

/// 端口验证辅助函数（int 版本，检查 1-65535 范围）
inline bool is_valid_port(int port) { return port > 0 && port <= 65535; }

/// sync_key 校验失败原因（通过时返回空字符串）
inline std::string get_sync_key_validation_error(const std::string& sync_key) {
    if (sync_key.empty()) {
        return "sync_key 不能为空";
    }
    if (sync_key.length() > 64) {
        return "sync_key 长度不能超过 64";
    }
    if (sync_key.find("..") != std::string::npos) {
        return "sync_key 不能包含连续的点号 '..'";
    }
    if (sync_key.find('/') != std::string::npos || sync_key.find('\\') != std::string::npos) {
        return "sync_key 不能包含路径分隔符 '/' 或 '\\'";
    }

    for (unsigned char ch : sync_key) {
        const bool is_ok = std::isalnum(ch) || ch == '_' || ch == '-';
        if (!is_ok) {
            return "sync_key 仅允许字母、数字、下划线和中划线";
        }
    }

    return "";
}

/// sync_key 验证
inline bool is_valid_sync_key(const std::string& sync_key) {
    return get_sync_key_validation_error(sync_key).empty();
}

/// 配置验证：检查所有字段的有效性，返回错误信息列表（空 = 全部通过）
std::vector<std::string> validate_config(const Config& config);

// 辅助函数：生成 UUID v4
std::string generate_uuid_v4();

// 辅助函数：加载配置或创建默认配置
Config load_config_or_create_default(const std::string& config_path = "config.json");

}  // namespace VeritasSync
