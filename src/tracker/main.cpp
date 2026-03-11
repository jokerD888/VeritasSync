#include "VeritasSync/tracker/TrackerServer.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <iostream>

#if defined(_WIN32)
#include <locale.h>
#include <windows.h>
#endif

std::shared_ptr<spdlog::logger> g_logger;

void init_tracker_logger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);

        g_logger = std::make_shared<spdlog::logger>("veritas_tracker", spdlog::sinks_init_list{console_sink});
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::info);

        spdlog::register_logger(g_logger);
        spdlog::set_default_logger(g_logger);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        exit(1);
    }
}

int main() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
#endif

    init_tracker_logger();
    try {
        boost::asio::io_context io_context;
        TrackerServer server(io_context, 9988);
        g_logger->info("Tracker server (JSON async) started on port 9988...");
        io_context.run();
    } catch (const std::exception& e) {
        g_logger->critical("Exception: {}", e.what());
    }
    return 0;
}
