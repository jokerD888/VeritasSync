#pragma once
#include <spdlog/spdlog.h>

#include <memory>

namespace VeritasSync {
extern std::shared_ptr<spdlog::logger> g_logger;
// 添加初始化函数声明
void init_logger();

}  // namespace VeritasSync