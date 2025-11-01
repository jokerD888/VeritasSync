#pragma once

#include <spdlog/spdlog.h>
#include <memory>

namespace VeritasSync {
    // 声明一个全局可访问的 logger 实例
    // 它将在 main.cpp 中被定义和初始化
    extern std::shared_ptr<spdlog::logger> g_logger;
}
