#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace VeritasSync {

// 单个同步任务的结构
struct SyncTask {
    std::string sync_key;
    std::string role;  // "source" or "destination"
    // 移除: p2p_port 不再由 config.json 指定，而是由 libjuice 自动选择
    // unsigned short p2p_port;
    std::string sync_folder;
};

// 顶级配置结构
struct Config {
    std::string tracker_host = "127.0.0.1";
    unsigned short tracker_port = 9988;

    // --- 新增：TURN 服务器配置 ---
    std::string turn_host;
    unsigned short turn_port = 3478;  // 默认
    std::string turn_username;
    std::string turn_password;
    // ----------------------------

    std::vector<SyncTask> tasks;
};

// --- nlohmann/json 集成 ---

inline void to_json(nlohmann::json& j, const SyncTask& task) {
    j = nlohmann::json{{"sync_key", task.sync_key},
                       {"role", task.role},
                       //    {"p2p_port", task.p2p_port}, // 移除
                       {"sync_folder", task.sync_folder}};
}

inline void from_json(const nlohmann::json& j, SyncTask& task) {
    j.at("sync_key").get_to(task.sync_key);
    j.at("role").get_to(task.role);
    // j.at("p2p_port").get_to(task.p2p_port); // 移除
    j.at("sync_folder").get_to(task.sync_folder);
}

inline void to_json(nlohmann::json& j, const Config& config) {
    j = nlohmann::json{{"tracker_host", config.tracker_host},
                       {"tracker_port", config.tracker_port},
                       {"turn_host", config.turn_host},          // 新增
                       {"turn_port", config.turn_port},          // 新增
                       {"turn_username", config.turn_username},  // 新增
                       {"turn_password", config.turn_password},  // 新增
                       {"tasks", config.tasks}};
}

inline void from_json(const nlohmann::json& j, Config& config) {
    j.at("tracker_host").get_to(config.tracker_host);
    j.at("tracker_port").get_to(config.tracker_port);

    // --- 新增：加载 TURN (如果存在) ---
    if (j.contains("turn_host")) j.at("turn_host").get_to(config.turn_host);
    if (j.contains("turn_port")) j.at("turn_port").get_to(config.turn_port);
    if (j.contains("turn_username")) j.at("turn_username").get_to(config.turn_username);
    if (j.contains("turn_password")) j.at("turn_password").get_to(config.turn_password);
    // ---------------------------------

    j.at("tasks").get_to(config.tasks);
}

// 辅助函数：加载配置或创建默认配置
inline Config load_config_or_create_default(const std::string& config_path = "config.json") {
    std::ifstream f(config_path);
    if (f.good()) {
        nlohmann::json j;
        f >> j;
        return j.get<Config>();
    } else {
        Config defaultConfig;
        // --- 新增：示例 TURN ---
        defaultConfig.turn_host = "your_turn_server.com";
        defaultConfig.turn_username = "user";
        defaultConfig.turn_password = "pass";
        // ----------------------

        // 移除 p2p_port
        defaultConfig.tasks.push_back({"my-key-1", "source", "./SyncNode_A"});
        defaultConfig.tasks.push_back({"my-key-1", "destination", "./SyncNode_B"});

        std::ofstream o(config_path);
        o << nlohmann::json(defaultConfig).dump(4) << std::endl;

        throw std::runtime_error(
            "Config file not found. A default 'config.json' has been created. Please edit it and restart.");
    }
}

}  // namespace VeritasSync
