#include "VeritasSync/common/Config.h"
#include "VeritasSync/common/Logger.h"

#include <filesystem>

namespace VeritasSync {

std::vector<std::string> validate_config(const Config& config) {
    std::vector<std::string> errors;

    // --- 端口范围验证 ---
    auto check_port = [&](const std::string& name, unsigned short port) {
        if (!is_valid_port(port)) {
            errors.push_back(name + " 端口无效 (必须在 1-65535 之间)");
        }
    };
    check_port("tracker_port", config.tracker_port);
    check_port("stun_port", config.stun_port);
    check_port("webui_port", config.webui_port);
    // turn_port 仅在 turn_host 非空时校验
    if (!config.turn_host.empty()) {
        check_port("turn_port", config.turn_port);
    }

    // --- 必填字段验证 ---
    if (config.tracker_host.empty()) {
        errors.push_back("tracker_host 不能为空");
    }
    if (config.stun_host.empty()) {
        errors.push_back("stun_host 不能为空");
    }
    if (config.device_id.empty()) {
        errors.push_back("device_id 不能为空（应自动生成）");
    }

    // --- 日志级别验证 ---
    {
        static const std::vector<std::string> valid_levels = {"debug", "info", "warn", "warning", "error", "err", "critical", "off"};
        bool valid = false;
        for (const auto& l : valid_levels) {
            if (config.log_level == l) { valid = true; break; }
        }
        if (!valid) {
            errors.push_back("log_level 值无效 ('" + config.log_level + "'), 有效值: debug, info, warn, error, critical, off");
        }
    }

    // --- 性能参数范围验证 ---
    if (config.kcp_update_interval_ms < 5 || config.kcp_update_interval_ms > 500) {
        errors.push_back("kcp_update_interval_ms 应在 5-500 之间 (当前: " + std::to_string(config.kcp_update_interval_ms) + ")");
    }
    if (config.chunk_size < 1024 || config.chunk_size > 1048576) {
        errors.push_back("chunk_size 应在 1024-1048576 (1KB-1MB) 之间 (当前: " + std::to_string(config.chunk_size) + ")");
    }
    if (config.kcp_window_size < 16 || config.kcp_window_size > 4096) {
        errors.push_back("kcp_window_size 应在 16-4096 之间 (当前: " + std::to_string(config.kcp_window_size) + ")");
    }

    // --- 同步任务验证 ---
    for (size_t i = 0; i < config.tasks.size(); ++i) {
        const auto& task = config.tasks[i];
        std::string prefix = "tasks[" + std::to_string(i) + "]: ";
        const std::string sync_key_error = get_sync_key_validation_error(task.sync_key);
        if (!sync_key_error.empty()) {
            errors.push_back(prefix + sync_key_error);
        }
        if (task.role != "source" && task.role != "destination") {
            errors.push_back(prefix + "role 必须是 'source' 或 'destination' (当前: '" + task.role + "')");
        }
        if (task.sync_folder.empty()) {
            errors.push_back(prefix + "sync_folder 不能为空");
        }
    }

    return errors;
}

std::string generate_uuid_v4() {
    try {
        boost::uuids::random_generator gen;
        boost::uuids::uuid uuid = gen();
        std::ostringstream ss;
        ss << uuid;
        return ss.str();
    } catch (const std::exception& e) {
        // LOGIC-002: 处理 random_generator 可能的异常（如随机数源不可用）
        if (g_logger) {
            g_logger->error("[Config] UUID生成失败: {}", e.what());
        }
        return "";
    } catch (...) {
        if (g_logger) {
            g_logger->error("[Config] UUID生成失败: 未知异常");
        }
        return "";
    }
}

Config load_config_or_create_default(const std::string& config_path) {
    std::ifstream f(config_path);
    bool need_save = false;
    Config config;
    
    if (f.good()) {
        try {
            nlohmann::json j;
            f >> j;
            f.close();
            config = j.get<Config>();
            
            // 如果配置文件中没有 device_id，生成一个并回写
            if (config.device_id.empty()) {
                config.device_id = generate_uuid_v4();
                need_save = true;
            }
        } catch (const std::exception& e) {
            // 【修复】配置文件损坏时不崩溃，备份损坏文件并创建默认配置
            f.close();
            
            std::string backup_path = config_path + ".bak";
            std::error_code ec;
            std::filesystem::rename(config_path, backup_path, ec);
            
            if (g_logger) {
                g_logger->error("[Config] 配置文件解析失败: {}，已备份到 {}，将使用默认配置",
                               e.what(), backup_path);
            }
            
            // 使用默认配置
            // LOGIC-004: Config{} 已包含所有默认值，只需显式设置需要动态生成的字段
            config = Config{};
            config.device_id = generate_uuid_v4();  // 动态生成唯一设备ID
            need_save = true;
        }
    } else {
        // 首次启动，创建默认配置
        config.device_id = generate_uuid_v4();
        
        // TURN 默认留空，防止连接无效地址导致延迟
        config.turn_host = "";
        config.turn_port = 3478;
        config.turn_username = "";
        config.turn_password = "";

        // STUN 默认使用 Google 的，比较稳定
        config.stun_host = "stun.l.google.com";
        config.stun_port = 19302;

        config.tasks = {};
        need_save = true;
    }
    
    // 回写配置文件（如果有新生成的 device_id 或是新文件）
    if (need_save) {
        std::ofstream o(config_path);
        o << nlohmann::json(config).dump(4) << std::endl;
    }
    
    return config;
}

}  // namespace VeritasSync
