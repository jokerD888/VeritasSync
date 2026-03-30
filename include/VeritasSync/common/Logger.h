#pragma once
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace VeritasSync {
extern std::shared_ptr<spdlog::logger> g_logger;
// 添加初始化函数声明（参数可从 Config::Logging 注入）
void init_logger(size_t max_file_size = 5 * 1024 * 1024,
                 size_t max_files = 3,
                 size_t thread_pool_size = 8192);

/// 根据字符串设置日志级别（支持 "debug", "info", "warn", "error", "critical", "off"）
void set_log_level(const std::string& level);

}  // namespace VeritasSync