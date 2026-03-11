#include "VeritasSync/common/Logger.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>
#include <unordered_map>

namespace VeritasSync {
// 定义全局 logger 变量
std::shared_ptr<spdlog::logger> g_logger;

void init_logger() {
    // 幂等性检查：如果已经初始化过，直接返回
    if (g_logger) {
        return;
    }
    
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::info);  // 调试级别: info
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("veritas_sync.log", 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::info);     // 调试级别: info

        spdlog::init_thread_pool(8192, 1);

        g_logger =
            std::make_shared<spdlog::async_logger>("veritas_sync", spdlog::sinks_init_list{console_sink, file_sink},
                                                   spdlog::thread_pool(), spdlog::async_overflow_policy::block);

        g_logger->set_level(spdlog::level::info);      // 调试级别: info
        g_logger->flush_on(spdlog::level::info);       // info 级别就刷新

        spdlog::register_logger(g_logger);
        spdlog::set_default_logger(g_logger);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        // 在测试环境中不应该退出，改为静默处理
        // exit(1);
    }
}
void set_log_level(const std::string& level) {
    if (!g_logger) return;

    static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
        {"debug", spdlog::level::debug},
        {"info", spdlog::level::info},
        {"warn", spdlog::level::warn},
        {"warning", spdlog::level::warn},
        {"error", spdlog::level::err},
        {"err", spdlog::level::err},
        {"critical", spdlog::level::critical},
        {"off", spdlog::level::off},
    };

    auto it = level_map.find(level);
    if (it != level_map.end()) {
        g_logger->set_level(it->second);
        // 同步设置所有 sink 的级别
        for (auto& sink : g_logger->sinks()) {
            sink->set_level(it->second);
        }
        g_logger->info("[Logger] 日志级别已设置为: {}", level);
    } else {
        g_logger->warn("[Logger] 未知的日志级别 '{}', 保持当前级别 (info)", level);
    }
}

}  // namespace VeritasSync