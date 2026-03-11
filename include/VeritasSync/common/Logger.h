#pragma once
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace VeritasSync {
extern std::shared_ptr<spdlog::logger> g_logger;
// 添加初始化函数声明
void init_logger();

/// 根据字符串设置日志级别（支持 "debug", "info", "warn", "error", "critical", "off"）
void set_log_level(const std::string& level);

}  // namespace VeritasSync