#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <boost/asio.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/Config.h"
#include "VeritasSync/EncodingUtils.h"  // 引入编码工具
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"
#include "VeritasSync/TrayIcon.h"  // 引入托盘图标
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

class SyncNode {
public:
    SyncNode(VeritasSync::SyncTask task, const VeritasSync::Config& global_config)
        : m_task(std::move(task)), m_global_config(global_config) {}

    // --- Getter 方法，供胶水层获取状态 ---
    std::shared_ptr<VeritasSync::P2PManager> get_p2p() { return m_p2p_manager; }
    std::string get_key() const { return m_task.sync_key; }
    // ----------------------------------------

    void start() {
        VeritasSync::g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
        VeritasSync::g_logger->info("[Config] Role: {}", m_task.role);
        // 这里为了日志输出不乱码，仍然打印原始字符串
        VeritasSync::g_logger->info("[Config] Sync Folder: {}", m_task.sync_folder);

        // 1. 使用 EncodingUtils 转换路径，防止中文路径导致崩溃
        std::filesystem::path sync_path = VeritasSync::Utf8ToPath(m_task.sync_folder);

        VeritasSync::SyncRole role;
        bool is_source;
        if (m_task.role == "source") {
            role = VeritasSync::SyncRole::Source;
            is_source = true;
        } else if (m_task.role == "destination") {
            role = VeritasSync::SyncRole::Destination;
            is_source = false;
        } else {
            VeritasSync::g_logger->error("Invalid role: '{}' for task '{}'.", m_task.role, m_task.sync_key);
            return;
        }

        // 2. 使用转换后的 sync_path 操作文件系统
        // 确保同步目录存在（不清空已有文件，支持重启后继续工作）
        std::error_code ec;
        if (!std::filesystem::exists(sync_path, ec)) {
            std::filesystem::create_directories(sync_path, ec);
            if (ec) {
                VeritasSync::g_logger->error("[SyncNode] 创建同步目录失败: {}", ec.message());
                return;  // 目录创建失败则终止该任务
            }
            VeritasSync::g_logger->info("[SyncNode] 创建同步目录成功");
        } else {
            VeritasSync::g_logger->info("[SyncNode] 使用现有同步目录");
        }

        // 1. 创建 P2PManager (它有自己的线程)
        m_p2p_manager = VeritasSync::P2PManager::create();

        // 2. 创建 TrackerClient (它有自己的线程)
        m_tracker_client =
            std::make_shared<VeritasSync::TrackerClient>(m_global_config.tracker_host, m_global_config.tracker_port);

        // 3. 互相注入依赖
        m_tracker_client->set_p2p_manager(m_p2p_manager.get());
        m_p2p_manager->set_tracker_client(m_tracker_client.get());

        // 4. 配置 P2PManager
        m_p2p_manager->set_role(role);
        m_p2p_manager->set_encryption_key(m_task.sync_key);
        // [新增] 注入同步模式 (用于支持双向同步广播)
        m_p2p_manager->set_mode(m_task.mode);

        // --- 配置 STUN 服务器 ---
        if (!m_global_config.stun_host.empty()) {
            VeritasSync::g_logger->info("[Config] Using STUN server at {}:{}", m_global_config.stun_host,
                                        m_global_config.stun_port);
            m_p2p_manager->set_stun_config(m_global_config.stun_host, m_global_config.stun_port);
        } else {
            VeritasSync::g_logger->warn("[Config] No STUN server configured. P2P NAT traversal may fail!");
        }

        // --- 配置 TURN 服务器 ---
        if (!m_global_config.turn_host.empty()) {
            VeritasSync::g_logger->info("[Config] Using TURN server at {}:{}", m_global_config.turn_host,
                                        m_global_config.turn_port);
            m_p2p_manager->set_turn_config(m_global_config.turn_host, m_global_config.turn_port,
                                           m_global_config.turn_username, m_global_config.turn_password);
        } else {
            VeritasSync::g_logger->info("[Config] No TURN server configured.");
        }

        // 5. 创建 StateManager
        // 使用 Utf8ToPath 转换路径
        m_state_manager = std::make_unique<VeritasSync::StateManager>(m_task.sync_folder, *m_p2p_manager, is_source);

        // 6. 注入 StateManager
        m_p2p_manager->set_state_manager(m_state_manager.get());

        // 7. 初始扫描
        m_state_manager->scan_directory();

        // 9. --- 启动信令连接 ---
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

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // 1. 设置控制台输入输出代码页为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 2. 启用 ANSI 转义序列支持 (让 spdlog 的颜色在 Windows 10+ 上正常显示)
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

    // 在后台线程启动阻塞的 listen 循环
    std::thread ui_thread([&web_ui]() { web_ui.start(); });

    // 创建同步任务节点
    std::vector<std::unique_ptr<SyncNode>> nodes;
    for (const auto& task : config.tasks) {
        auto node = std::make_unique<SyncNode>(task, config);
        node->start();
        nodes.push_back(std::move(node));
    }

    // --- 注入状态提供者给 WebUI ---
    // 这是一个胶水逻辑：将底层的 P2P 状态适配为 WebUI 需要的 JSON 格式
    web_ui.set_status_provider([&nodes]() {
        std::vector<nlohmann::json> result;
        for (const auto& node : nodes) {
            auto p2p = node->get_p2p();
            if (p2p) {
                // 调用 P2PManager 的线程安全接口获取当前下载列表
                auto downloads = p2p->get_active_downloads();
                for (const auto& item : downloads) {
                    nlohmann::json j;
                    j["key"] = node->get_key();
                    j["path"] = item.path;
                    j["total"] = item.total_chunks;
                    j["recv"] = item.received_chunks;
                    j["progress"] = item.progress;
                    result.push_back(j);
                }
            }
        }
        return result;
    });
    // -----------------------------------

#if defined(_WIN32)
    // --- 托盘图标逻辑 (仅 Windows) ---
    VeritasSync::TrayIcon tray;

    // 1. 初始化图标
    if (!tray.init("VeritasSync - P2P 同步节点")) {
        VeritasSync::g_logger->error("无法创建系统托盘图标");
    }

    // 2. 添加菜单：打开 WebUI
    tray.add_menu_item("🌐 打开控制台", []() {
        VeritasSync::WebUIServer::open_url("http://127.0.0.1:8800");  // <--- 使用 open_url
    });

    // 3. 添加菜单：打开同步目录 (如果有任务)
    if (!config.tasks.empty()) {
        tray.add_separator();
        // 仅展示第一个任务的目录，作为快捷入口
        std::string first_path = config.tasks[0].sync_folder;
        tray.add_menu_item("📂 打开文件夹",
                           [first_path]() { VeritasSync::WebUIServer::open_folder_in_os(first_path); });
    }

    tray.add_separator();

    // 4. 添加菜单：开机自启
    bool is_auto = VeritasSync::TrayIcon::is_autostart_enabled();
    tray.add_menu_item(
        "🚀 开机自启",
        [&tray]() {
            bool current = VeritasSync::TrayIcon::is_autostart_enabled();
            VeritasSync::TrayIcon::set_autostart(!current);

            // 提示用户
            std::wstring msg = !current ? L"已开启开机自启" : L"已关闭开机自启";
            MessageBoxW(NULL, msg.c_str(), L"VeritasSync", MB_OK | MB_ICONINFORMATION);

            // 注意：这里实际上应该动态更新菜单的 checked 状态，
            // 但为了简化实现，目前我们只在启动时读取一次。
            // 用户重启程序后菜单状态会更新。
        },
        is_auto);

    tray.add_separator();

    // 5. 添加菜单：退出
    tray.add_menu_item("🛑 退出程序", [&tray, &web_ui]() {
        if (MessageBoxW(NULL, L"确定要退出同步服务吗？", L"VeritasSync", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            web_ui.stop();
            tray.quit();
        }
    });

    VeritasSync::g_logger->info("系统托盘已启动。程序正在后台运行。");

    // 6. 进入 Windows 消息循环 (阻塞主线程，替代 sleep)
    tray.run_loop();

#else
    // Linux/Mac 继续使用原来的 Sleep 循环
    VeritasSync::g_logger->info("\n--- 所有同步任务已启动。Web UI: http://127.0.0.1:8800 | 按 Ctrl+C 退出 ---");
    std::this_thread::sleep_for(std::chrono::hours(24000));  // 无限等待
#endif

    // 清理
    web_ui.stop();
    if (ui_thread.joinable()) ui_thread.join();

    VeritasSync::g_logger->info("--- Shutting down. ---");
    spdlog::shutdown();
    return 0;
}