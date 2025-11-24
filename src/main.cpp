#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"  // 如果这里需要引用类型定义
#include "VeritasSync/SyncNode.h"    // 引入新类
#include "VeritasSync/TrayIcon.h"
#include "VeritasSync/WebUI.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#pragma comment(lib, "shell32.lib")
#endif

// --- 全局变量：管理活跃节点 ---
std::vector<std::unique_ptr<VeritasSync::SyncNode>> g_active_nodes;
std::mutex g_nodes_mutex;

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // 1. 设置控制台输入输出代码页为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 2. 启用 ANSI 转义序列支持
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif

    VeritasSync::init_logger();
    VeritasSync::g_logger->info("--- Veritas Sync Node Starting Up ---");

    VeritasSync::Config config;
    try {
        config = VeritasSync::load_config_or_create_default("config.json");
    } catch (const std::exception& e) {
        VeritasSync::g_logger->warn("{}", e.what());
        VeritasSync::g_logger->info("Please edit config.json and restart. Shutting down.");
        spdlog::shutdown();
        return 1;
    }
    VeritasSync::g_logger->info("Configuration loaded. Tracker at {}:{}. Found {} task(s).", config.tracker_host,
                                config.tracker_port, config.tasks.size());

    // 启动 Web UI
    VeritasSync::WebUIServer web_ui(8800, "config.json");

    // --- 设置动态添加任务回调 ---
    web_ui.set_on_task_add([](const VeritasSync::SyncTask& new_task, const VeritasSync::Config& current_cfg) {
        VeritasSync::g_logger->info("[Main] 收到动态添加任务请求: {}", new_task.sync_key);
        std::lock_guard<std::mutex> lock(g_nodes_mutex);

        auto node = std::make_unique<VeritasSync::SyncNode>(new_task, current_cfg);
        node->start();
        g_active_nodes.push_back(std::move(node));
    });

    // --- 启动初始任务 ---
    {
        std::lock_guard<std::mutex> lock(g_nodes_mutex);
        for (const auto& task : config.tasks) {
            auto node = std::make_unique<VeritasSync::SyncNode>(task, config);
            node->start();
            g_active_nodes.push_back(std::move(node));
        }
    }

    // --- 注入状态提供者给 WebUI ---
    web_ui.set_status_provider([]() {
        std::vector<nlohmann::json> result;
        std::lock_guard<std::mutex> lock(g_nodes_mutex);

        for (const auto& node : g_active_nodes) {
            auto p2p = node->get_p2p();
            if (p2p) {
                auto transfers = p2p->get_active_transfers();
                for (const auto& item : transfers) {
                    nlohmann::json j;
                    j["key"] = node->get_key();
                    j["root"] = node->get_root_path();
                    j["path"] = item.path;
                    j["total"] = item.total_chunks;
                    j["done"] = item.processed_chunks;
                    j["progress"] = item.progress;
                    j["type"] = item.is_upload ? "upload" : "download";
                    j["speed"] = item.speed;
                    result.push_back(j);
                }
            }
        }
        return result;
    });

    // 在后台线程启动 WebUI
    std::thread ui_thread([&web_ui]() { web_ui.start(); });

#if defined(_WIN32)
    // --- 托盘图标逻辑 ---
    VeritasSync::TrayIcon tray;

    if (!tray.init("VeritasSync - P2P 同步节点")) {
        VeritasSync::g_logger->error("无法创建系统托盘图标");
    }

    tray.add_menu_item("🌐 打开控制台", []() { VeritasSync::WebUIServer::open_url("http://127.0.0.1:8800"); });

    if (!config.tasks.empty()) {
        tray.add_separator();
        std::string first_path = config.tasks[0].sync_folder;
        tray.add_menu_item("📂 打开文件夹",
                           [first_path]() { VeritasSync::WebUIServer::open_folder_in_os(first_path); });
    }

    tray.add_separator();

    tray.add_menu_item(
        "🚀 开机自启",
        [&tray]() {
            bool current = VeritasSync::TrayIcon::is_autostart_enabled();
            VeritasSync::TrayIcon::set_autostart(!current);
            VeritasSync::g_logger->info("用户切换开机自启为: {}", !current);
        },
        []() -> bool { return VeritasSync::TrayIcon::is_autostart_enabled(); });

    tray.add_separator();

    tray.add_menu_item("🛑 退出程序", [&tray, &web_ui]() {
        if (MessageBoxW(NULL, L"确定要退出同步服务吗？", L"VeritasSync", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            web_ui.stop();
            tray.quit();
        }
    });

    VeritasSync::g_logger->info("系统托盘已启动。程序正在后台运行。");
    tray.run_loop();

#else
    VeritasSync::g_logger->info("\n--- 所有同步任务已启动。Web UI: http://127.0.0.1:8800 | 按 Ctrl+C 退出 ---");
    std::this_thread::sleep_for(std::chrono::hours(24000));
#endif

    // 清理
    web_ui.stop();
    if (ui_thread.joinable()) ui_thread.join();

    VeritasSync::g_logger->info("--- Shutting down. ---");
    spdlog::shutdown();
    return 0;
}