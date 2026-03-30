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
    check_port("tracker_port", config.network.tracker_port);
    check_port("stun_port", config.network.stun_port);
    check_port("webui_port", config.webui.port);
    // turn_port 仅在 turn_host 非空时校验
    if (!config.network.turn_host.empty()) {
        check_port("turn_port", config.network.turn_port);
    }

    // --- 必填字段验证 ---
    if (config.network.tracker_host.empty()) {
        errors.push_back("tracker_host 不能为空");
    }
    if (config.network.stun_host.empty()) {
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
            if (config.logging.level == l) { valid = true; break; }
        }
        if (!valid) {
            errors.push_back("log_level 值无效 ('" + config.logging.level + "'), 有效值: debug, info, warn, error, critical, off");
        }
    }

    // --- 性能参数范围验证 ---
    if (config.kcp.update_interval_ms < 5 || config.kcp.update_interval_ms > 500) {
        errors.push_back("kcp_update_interval_ms 应在 5-500 之间 (当前: " + std::to_string(config.kcp.update_interval_ms) + ")");
    }
    if (config.transfer.chunk_size < 1024 || config.transfer.chunk_size > 1048576) {
        errors.push_back("chunk_size 应在 1024-1048576 (1KB-1MB) 之间 (当前: " + std::to_string(config.transfer.chunk_size) + ")");
    }
    if (config.kcp.window_size < 16 || config.kcp.window_size > 4096) {
        errors.push_back("kcp_window_size 应在 16-4096 之间 (当前: " + std::to_string(config.kcp.window_size) + ")");
    }

    // --- 额外 STUN 服务器验证 ---
    for (size_t i = 0; i < config.network.extra_stun_servers.size(); ++i) {
        const auto& server = config.network.extra_stun_servers[i];
        std::string prefix = "extra_stun_servers[" + std::to_string(i) + "]: ";
        if (server.host.empty()) {
            errors.push_back(prefix + "host 不能为空");
        }
        if (!is_valid_port(server.port)) {
            errors.push_back(prefix + "port 无效 (必须在 1-65535 之间)");
        }
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
        // thread_local: 避免每次调用都重新构造 + 种子化 PRNG
        thread_local boost::uuids::random_generator gen;
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
    namespace fs = std::filesystem;

    // advanced.json 与 config.json 同目录
    fs::path config_dir = fs::path(config_path).parent_path();
    std::string advanced_path = (config_dir / "advanced.json").string();

    bool need_save_config = false;
    bool need_save_advanced = false;
    Config config;

    // ─── 加载 config.json ───
    {
        std::ifstream f(config_path);
        if (f.good()) {
            try {
                nlohmann::json j;
                f >> j;
                f.close();
                config_from_json(j, config);

                if (config.device_id.empty()) {
                    config.device_id = generate_uuid_v4();
                    need_save_config = true;
                }
            } catch (const std::exception& e) {
                f.close();
                std::string backup_path = config_path + ".bak";
                std::error_code ec;
                fs::rename(config_path, backup_path, ec);
                if (g_logger) {
                    g_logger->error("[Config] config.json 解析失败: {}，已备份到 {}",
                                   e.what(), backup_path);
                }
                config = Config{};
                config.device_id = generate_uuid_v4();
                need_save_config = true;
                need_save_advanced = true;
            }
        } else {
            // 首次启动
            config = Config{};
            config.device_id = generate_uuid_v4();
            need_save_config = true;
            need_save_advanced = true;
        }
    }

    // ─── 加载 advanced.json（可选，不存在则用默认值）───
    {
        std::ifstream f(advanced_path);
        if (f.good()) {
            try {
                nlohmann::json j;
                f >> j;
                f.close();
                advanced_from_json(j, config);
            } catch (const std::exception& e) {
                f.close();
                if (g_logger) {
                    g_logger->warn("[Config] advanced.json 解析失败: {}，将使用默认值", e.what());
                }
                need_save_advanced = true;
            }
        } else {
            // advanced.json 不存在：首次启动生成，或用户不需要调优
            need_save_advanced = true;
        }
    }

    // ─── 回写文件 ───
    if (need_save_config) {
        // 确保目录存在
        if (!config_dir.empty()) {
            std::error_code ec;
            fs::create_directories(config_dir, ec);
        }
        std::ofstream o(config_path);
        o << config_to_json(config).dump(4) << std::endl;
    }
    if (need_save_advanced) {
        if (!config_dir.empty()) {
            std::error_code ec;
            fs::create_directories(config_dir, ec);
        }
        std::ofstream o(advanced_path);
        o << advanced_to_json(config).dump(4) << std::endl;
    }

    return config;
}

}  // namespace VeritasSync
