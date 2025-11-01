#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>  // 用于 std::runtime_error
#include <string>
#include <vector>

namespace VeritasSync {

// 单个同步任务的结构
struct SyncTask {
    std::string sync_key;
    std::string role;  // "source" or "destination"
    unsigned short p2p_port;
    std::string sync_folder;
};

// 顶级配置结构
struct Config {
    std::string tracker_host = "127.0.0.1";
    unsigned short tracker_port = 9988;
    std::vector<SyncTask> tasks;
};

// --- nlohmann/json 集成 ---

inline void to_json(nlohmann::json& j, const SyncTask& task) {
    j = nlohmann::json{{"sync_key", task.sync_key},
                       {"role", task.role},
                       {"p2p_port", task.p2p_port},
                       {"sync_folder", task.sync_folder}};
}

inline void from_json(const nlohmann::json& j, SyncTask& task) {
    j.at("sync_key").get_to(task.sync_key);
    j.at("role").get_to(task.role);
    j.at("p2p_port").get_to(task.p2p_port);
    j.at("sync_folder").get_to(task.sync_folder);
}

inline void to_json(nlohmann::json& j, const Config& config) {
    j = nlohmann::json{
        {"tracker_host", config.tracker_host}, {"tracker_port", config.tracker_port}, {"tasks", config.tasks}};
}

inline void from_json(const nlohmann::json& j, Config& config) {
    j.at("tracker_host").get_to(config.tracker_host);
    j.at("tracker_port").get_to(config.tracker_port);
    j.at("tasks").get_to(config.tasks);
}

// 辅助函数：加载配置或创建默认配置
inline Config load_config_or_create_default(const std::string& config_path = "config.json") {
    std::ifstream f(config_path);
    if (f.good()) {
        // 文件存在，解析它
        nlohmann::json j;
        f >> j;
        return j.get<Config>();
    } else {
        // 文件不存在，创建默认示例
        Config defaultConfig;
        defaultConfig.tasks.push_back({"my-key-1", "source", 10001, "./SyncNode_A"});
        defaultConfig.tasks.push_back({"my-key-1", "destination", 10002, "./SyncNode_B"});

        std::ofstream o(config_path);
        o << nlohmann::json(defaultConfig).dump(4) << std::endl;

        // 抛出异常通知 main 函数退出
        throw std::runtime_error(
            "Config file not found. A default 'config.json' has been created. Please edit it and restart.");
    }
}

}  // namespace VeritasSync
