#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/Config.h"
#include "VeritasSync/EncodingUtils.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"
#include "VeritasSync/TrayIcon.h"
#include "VeritasSync/WebUI.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace VeritasSync {
std::shared_ptr<spdlog::logger> g_logger;
}

// 初始化日志系统
void init_logger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("veritas_sync.log", 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::debug);
        spdlog::init_thread_pool(8192, 1);
        VeritasSync::g_logger =
            std::make_shared<spdlog::async_logger>("veritas_sync", spdlog::sinks_init_list{console_sink, file_sink},
                                                   spdlog::thread_pool(), spdlog::async_overflow_policy::block);
        VeritasSync::g_logger->set_level(spdlog::level::debug);
        VeritasSync::g_logger->flush_on(spdlog::level::info);
        spdlog::register_logger(VeritasSync::g_logger);
        spdlog::set_default_logger(VeritasSync::g_logger);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        exit(1);
    }
}

// --- 同步节点类 ---
class SyncNode {
public:
    SyncNode(VeritasSync::SyncTask task, const VeritasSync::Config& global_config)
        : m_task(std::move(task)), m_global_config(global_config) {}

    // Getter 方法，供胶水层获取状态
    std::shared_ptr<VeritasSync::P2PManager> get_p2p() { return m_p2p_manager; }
    std::string get_key() const { return m_task.sync_key; }

    std::string get_root_path() const { return m_task.sync_folder; }

    void start() {
        VeritasSync::g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
        VeritasSync::g_logger->info("[Config] Role: {}", m_task.role);
        VeritasSync::g_logger->info("[Config] Sync Folder: {}", m_task.sync_folder);

        // 1. 使用 EncodingUtils 转换路径
        std::filesystem::path sync_path = VeritasSync::Utf8ToPath(m_task.sync_folder);

        VeritasSync::SyncRole role;
        if (m_task.role == "source") {
            role = VeritasSync::SyncRole::Source;
        } else if (m_task.role == "destination") {
            role = VeritasSync::SyncRole::Destination;
        } else {
            VeritasSync::g_logger->error("Invalid role: '{}' for task '{}'.", m_task.role, m_task.sync_key);
            return;
        }

        // [核心修正] 开启文件监控的条件：我是 Source 或者 任务是双向同步
        // 即使是 Destination，在双向模式下也需要监控文件变化以便发送给对方
        bool enable_watcher =
            (role == VeritasSync::SyncRole::Source) || (m_task.mode == VeritasSync::SyncMode::BiDirectional);

        // 2. 确保目录存在
        std::error_code ec;
        if (!std::filesystem::exists(sync_path, ec)) {
            std::filesystem::create_directories(sync_path, ec);
            if (ec) {
                VeritasSync::g_logger->error("[SyncNode] 创建同步目录失败: {}", ec.message());
                return;
            }
            VeritasSync::g_logger->info("[SyncNode] 创建同步目录成功");
        } else {
            VeritasSync::g_logger->info("[SyncNode] 使用现有同步目录");
        }

        // 3. 创建 P2PManager
        m_p2p_manager = VeritasSync::P2PManager::create();

        // 4. 创建 TrackerClient
        m_tracker_client =
            std::make_shared<VeritasSync::TrackerClient>(m_global_config.tracker_host, m_global_config.tracker_port);

        // 5. 互相注入依赖
        m_tracker_client->set_p2p_manager(m_p2p_manager.get());
        m_p2p_manager->set_tracker_client(m_tracker_client.get());

        // 6. 配置 P2PManager
        m_p2p_manager->set_role(role);
        m_p2p_manager->set_encryption_key(m_task.sync_key);
        m_p2p_manager->set_mode(m_task.mode);

        // 配置 STUN
        if (!m_global_config.stun_host.empty()) {
            VeritasSync::g_logger->info("[Config] Using STUN server at {}:{}", m_global_config.stun_host,
                                        m_global_config.stun_port);
            m_p2p_manager->set_stun_config(m_global_config.stun_host, m_global_config.stun_port);
        }

        // 配置 TURN
        if (!m_global_config.turn_host.empty()) {
            VeritasSync::g_logger->info("[Config] Using TURN server at {}:{}", m_global_config.turn_host,
                                        m_global_config.turn_port);
            m_p2p_manager->set_turn_config(m_global_config.turn_host, m_global_config.turn_port,
                                           m_global_config.turn_username, m_global_config.turn_password);
        }
        m_p2p_manager->set_multi_wan_config(m_global_config.enable_multi_stun_probing, m_global_config.stun_list_url);

        // 7. 创建 StateManager (传入 sync_key 用于日志)
        // 第三个参数传入计算好的 enable_watcher，确保双向模式下两端都监控文件
        m_state_manager = std::make_unique<VeritasSync::StateManager>(m_task.sync_folder, *m_p2p_manager,
                                                                      enable_watcher, m_task.sync_key);

        // 8. 注入 StateManager (这会自动更新 TransferManager 内部的指针)
        m_p2p_manager->set_state_manager(m_state_manager.get());

        // 9. 初始扫描
        m_state_manager->scan_directory();

        // 10. 启动信令连接
        VeritasSync::g_logger->info("[{}] --- Phase 1: Contacting Tracker ---", m_task.sync_key);

        m_tracker_client->connect(m_task.sync_key, [this](std::vector<std::string> peer_list) {
            VeritasSync::g_logger->info("[{}] --- Phase 2: P2P (ICE) connection ---", m_task.sync_key);
            m_p2p_manager->connect_to_peers(peer_list);
        });
    }

private:
    VeritasSync::SyncTask m_task;
    VeritasSync::Config m_global_config;

    std::shared_ptr<VeritasSync::TrackerClient> m_tracker_client;
    std::shared_ptr<VeritasSync::P2PManager> m_p2p_manager;
    std::unique_ptr<VeritasSync::StateManager> m_state_manager;
};

// --- 全局变量：管理活跃节点 ---
std::vector<std::unique_ptr<SyncNode>> g_active_nodes;
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

    init_logger();
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

    // --- [新增] 设置动态添加任务回调 ---
    web_ui.set_on_task_add([](const VeritasSync::SyncTask& new_task, const VeritasSync::Config& current_cfg) {
        VeritasSync::g_logger->info("[Main] 收到动态添加任务请求: {}", new_task.sync_key);
        std::lock_guard<std::mutex> lock(g_nodes_mutex);

        auto node = std::make_unique<SyncNode>(new_task, current_cfg);
        node->start();
        g_active_nodes.push_back(std::move(node));
    });

    // --- 启动初始任务 ---
    {
        std::lock_guard<std::mutex> lock(g_nodes_mutex);
        for (const auto& task : config.tasks) {
            auto node = std::make_unique<SyncNode>(task, config);
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
                //  调用新的 get_active_transfers 接口，支持上传/下载区分
                auto transfers = p2p->get_active_transfers();
                for (const auto& item : transfers) {
                    nlohmann::json j;
                    j["key"] = node->get_key();
                    j["root"] = node->get_root_path();
                    j["path"] = item.path;
                    j["total"] = item.total_chunks;
                    j["done"] = item.processed_chunks;  // 改名后的字段
                    j["progress"] = item.progress;
                    j["type"] = item.is_upload ? "upload" : "download";  // 新增类型字段
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
    // --- 托盘图标逻辑 (仅 Windows) ---
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

    bool is_auto = VeritasSync::TrayIcon::is_autostart_enabled();
    tray.add_menu_item(
        "🚀 开机自启",
        [&tray]() {
            bool current = VeritasSync::TrayIcon::is_autostart_enabled();
            VeritasSync::TrayIcon::set_autostart(!current);
            std::wstring msg = !current ? L"已开启开机自启" : L"已关闭开机自启";
            MessageBoxW(NULL, msg.c_str(), L"VeritasSync", MB_OK | MB_ICONINFORMATION);
        },
        is_auto);

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