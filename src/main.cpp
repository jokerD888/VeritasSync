#include <atomic>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "VeritasSync/common/Config.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/P2PManager.h"
#include "VeritasSync/sync/SyncNode.h"
#include "VeritasSync/common/TrayIcon.h"
#include "VeritasSync/p2p/WebUI.h"

#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#pragma comment(lib, "shell32.lib")
#endif

// ═══════════════════════════════════════════════════════════════
// 配置文件路径解析（两级查找：便携模式 → 系统标准路径）
// ═══════════════════════════════════════════════════════════════

static std::filesystem::path get_exe_path() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::filesystem::path(buf);
#elif defined(__linux__)
    return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    _NSGetExecutablePath(buf, &size);
    return std::filesystem::canonical(buf);
#else
    return std::filesystem::current_path() / "veritas_sync";
#endif
}

static std::filesystem::path get_exe_directory() {
    return get_exe_path().parent_path();
}

static std::filesystem::path get_system_config_dir() {
#if defined(_WIN32)
    // %APPDATA%\VeritasSync (e.g. C:\Users\xxx\AppData\Roaming\VeritasSync)
    #pragma warning(suppress: 4996)  // getenv is safe in this context
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::filesystem::path(appdata) / "VeritasSync";
    return get_exe_directory();
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / "Library" / "Application Support" / "VeritasSync";
    return get_exe_directory();
#else
    // Linux: XDG_CONFIG_HOME or ~/.config/veritassync
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg) return std::filesystem::path(xdg) / "veritassync";
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".config" / "veritassync";
    return get_exe_directory();
#endif
}

/// 解析 config.json 路径：便携模式优先，系统路径兜底
static std::string resolve_config_path() {
    namespace fs = std::filesystem;
    const std::string filename = "config.json";

    // 1. 便携模式：exe 同目录下有 config.json → 直接用
    auto portable = get_exe_directory() / filename;
    if (fs::exists(portable)) {
        return portable.string();
    }

    // 2. 系统标准路径
    auto sys_dir = get_system_config_dir();
    auto sys_path = sys_dir / filename;
    if (fs::exists(sys_path)) {
        return sys_path.string();
    }

    // 3. 都没有 → 在 exe 目录创建（首次启动，便携模式默认）
    return portable.string();
}

// --- 全局变量：管理活跃节点 ---
// 注意：SyncNode 现在必须使用 shared_ptr（支持 enable_shared_from_this）
std::vector<std::shared_ptr<VeritasSync::SyncNode>> g_active_nodes;
std::mutex g_nodes_mutex;

// --- 全局停止标志 (用于优雅关闭) ---
std::atomic<bool> g_shutdown_requested{false};
// --- 全局重启标志 (用于清理完成后重启) ---
std::atomic<bool> g_restart_requested{false};

// --- 信号处理 (跨平台优雅关闭) ---
void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_shutdown_requested = true;
    }
}

bool relaunch_current_process() {
    auto exe = get_exe_path();
#if defined(_WIN32)
    std::wstring wpath = exe.wstring();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(wpath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return false;
#else
    std::string path_str = exe.string();
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        char* args[] = {path_str.data(), nullptr};
        execv(path_str.c_str(), args);
        _exit(1);
    }
    waitpid(pid, nullptr, WNOHANG);
    return true;
#endif
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
#if defined(_WIN32)
    // ===== 高 DPI 感知声明（必须在创建任何窗口之前调用）=====
    // 防止 IFileDialog 等 Shell 对话框在 2K/4K 屏幕上被 DPI 虚拟化缩放
    {
        HMODULE hUser32 = LoadLibraryW(L"user32.dll");
        if (hUser32) {
            // 优先使用 Win10 1703+ 的 Per-Monitor V2
            using SetDpiAwarenessContextFunc = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            auto pSetDpiAwarenessContext = reinterpret_cast<SetDpiAwarenessContextFunc>(
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
            if (pSetDpiAwarenessContext) {
                pSetDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            } else {
                // 回退到 Win8.1+ 的 SetProcessDpiAwareness
                HMODULE hShcore = LoadLibraryW(L"shcore.dll");
                if (hShcore) {
                    using SetDpiAwarenessFunc = HRESULT(WINAPI*)(int);
                    auto pSetDpiAwareness = reinterpret_cast<SetDpiAwarenessFunc>(
                        GetProcAddress(hShcore, "SetProcessDpiAwareness"));
                    if (pSetDpiAwareness) {
                        pSetDpiAwareness(2);  // PROCESS_PER_MONITOR_DPI_AWARE
                    }
                    FreeLibrary(hShcore);
                }
            }
            FreeLibrary(hUser32);
        }
    }

    HANDLE hMutex = nullptr;

    // ===== 单实例检查 =====
    // 使用命名互斥锁防止程序多开
    hMutex = CreateMutexW(NULL, TRUE, L"VeritasSync_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行，提示用户并退出
        MessageBoxW(NULL, 
                    L"VeritasSync 已经在运行中。\n\n请检查系统托盘区域。", 
                    L"VeritasSync", 
                    MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

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

    // 异常清理辅助：释放 Windows 单实例互斥锁 + 记录日志 + 关闭 spdlog
    auto fatal_cleanup = [&](const char* msg) {
#if defined(_WIN32)
        if (hMutex) {
            CloseHandle(hMutex);
            hMutex = nullptr;
        }
#endif
        if (VeritasSync::g_logger) {
            VeritasSync::g_logger->critical("{}", msg);
        } else {
            std::cerr << msg << std::endl;
        }
        spdlog::shutdown();
    };

    try {
    // 先加载配置（不需要日志），然后用配置参数初始化日志系统
    VeritasSync::Config config;
    std::string config_path = resolve_config_path();
    try {
        config = VeritasSync::load_config_or_create_default(config_path);
    } catch (const std::exception& e) {
        // 配置加载失败时使用默认参数初始化日志
        VeritasSync::init_logger();
        VeritasSync::g_logger->warn("{}", e.what());
        VeritasSync::g_logger->info("Please edit {} and restart. Shutting down.", config_path);
        spdlog::shutdown();
        return 1;
    }

    // 使用配置参数初始化日志系统
    VeritasSync::init_logger(
        static_cast<size_t>(config.logging.max_file_size_mb) * 1024 * 1024,
        static_cast<size_t>(config.logging.max_files),
        static_cast<size_t>(config.logging.thread_pool_size));
    VeritasSync::g_logger->info("--- Veritas Sync Node Starting Up ---");
    VeritasSync::g_logger->info("Configuration loaded from: {}", config_path);

    // 配置验证
    auto config_errors = VeritasSync::validate_config(config);
    if (!config_errors.empty()) {
        VeritasSync::g_logger->warn("配置校验发现 {} 个问题:", config_errors.size());
        for (const auto& err : config_errors) {
            VeritasSync::g_logger->warn("  - {}", err);
        }
        VeritasSync::g_logger->warn("请检查 {} 并修正上述问题。程序将使用当前配置继续运行。", config_path);
    }

    VeritasSync::g_logger->info("Configuration loaded. Tracker at {}:{}. Found {} task(s).", config.network.tracker_host,
                                config.network.tracker_port, config.tasks.size());

    // 注入高级配置参数到静态模块
    VeritasSync::Hashing::set_read_buffer_size(
        static_cast<size_t>(config.advanced.hash_read_buffer_size_kb) * 1024);

    // 根据配置设置日志级别 (修复: 之前 Config.log_level 未生效)
    VeritasSync::set_log_level(config.logging.level);

    // 注册信号处理 (跨平台: SIGINT=Ctrl+C, SIGTERM=kill)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 启动 Web UI (端口从配置读取，默认 8800)
    VeritasSync::WebUIServer web_ui(config.webui.port, "config.json");

    // --- B-02: 重启请求回调（由 main 执行优雅重启） ---
    web_ui.set_on_restart_request([&web_ui]() {
        VeritasSync::g_logger->info("[Main] 收到 WebUI 重启请求，准备优雅退出...");
        g_restart_requested = true;
        g_shutdown_requested = true;
        web_ui.stop();
    });

    // --- 设置动态添加任务回调 ---
    web_ui.set_on_task_add([](const VeritasSync::SyncTask& new_task, const VeritasSync::Config& current_cfg) -> bool {
        VeritasSync::g_logger->info("[Main] 收到动态添加任务请求: {}", new_task.sync_key);
        std::lock_guard<std::mutex> lock(g_nodes_mutex);

        // 使用工厂方法创建 SyncNode（必须是 shared_ptr）
        auto node = VeritasSync::SyncNode::create(new_task, current_cfg);
        // 【修复】检查start()返回值
        if (!node->start()) {
            VeritasSync::g_logger->error("[Main] 动态添加任务失败: {}", new_task.sync_key);
            return false;
        }
        g_active_nodes.push_back(node);
        return true;
    });

    // --- C-5: 设置动态删除任务回调 ---
    web_ui.set_on_task_remove([](const std::string& sync_key) {
        VeritasSync::g_logger->info("[Main] 收到删除任务请求: {}", sync_key);
        std::lock_guard<std::mutex> lock(g_nodes_mutex);

        // 通过 sync_key 查找并停止对应的 SyncNode
        auto it = std::find_if(g_active_nodes.begin(), g_active_nodes.end(),
            [&sync_key](const std::shared_ptr<VeritasSync::SyncNode>& node) {
                return node->get_key() == sync_key;
            });
        
        if (it != g_active_nodes.end()) {
            // 统一关闭职责：由 SyncNode::stop() 内部执行 graceful shutdown + 资源清理
            (*it)->stop();
            g_active_nodes.erase(it);
            VeritasSync::g_logger->info("[Main] 已停止并移除任务: {}", sync_key);
        } else {
            VeritasSync::g_logger->warn("[Main] 未找到对应的活跃节点: {}", sync_key);
        }
    });

    // --- 启动初始任务 ---
    {
        std::lock_guard<std::mutex> lock(g_nodes_mutex);
        for (const auto& task : config.tasks) {
            // 使用工厂方法创建 SyncNode（必须是 shared_ptr）
            auto node = VeritasSync::SyncNode::create(task, config);
            // 【修复】检查start()返回值，失败时不添加到活跃节点
            if (!node->start()) {
                VeritasSync::g_logger->error("[Main] 启动同步任务失败: {}", task.sync_key);
                continue;
            }
            g_active_nodes.push_back(node);
        }
    }

    // --- 注入状态提供者给 WebUI ---
    web_ui.set_status_provider([]() {
        nlohmann::json root;
        root["nodes"] = nlohmann::json::array();

        // 全局统计变量
        bool any_tracker_online = false;
        uint64_t global_done = 0;
        uint64_t global_total = 0;
        
        // 对等点统计
        nlohmann::json all_peers = nlohmann::json::array();
        int connected_count = 0;
        int connecting_count = 0;
        int failed_count = 0;

        std::lock_guard<std::mutex> lock(g_nodes_mutex);
        for (const auto& node : g_active_nodes) {
            nlohmann::json node_json;
            node_json["key"] = node->get_key();
            node_json["root"] = node->get_root_path();

            // 1. 获取 Tracker 状态
            if (node->is_tracker_online()) {
                any_tracker_online = true;
                node_json["online"] = true;
            } else {
                node_json["online"] = false;
            }

            // 2. 获取传输数据
            auto p2p = node->get_p2p();
            node_json["transfers"] = nlohmann::json::array();
            int node_connected_peers = 0;
            bool has_active_transfers = false;

            if (p2p) {
                // A. 累加统计数据
                auto stats = p2p->get_transfer_stats();
                global_done += stats.done;
                global_total += stats.total;

                // B. 获取活跃列表 (大文件)
                auto active_list = p2p->get_active_transfers();
                has_active_transfers = !active_list.empty();
                for (const auto& item : active_list) {
                    nlohmann::json t;
                    t["path"] = item.path;
                    t["progress"] = item.progress;
                    t["speed"] = item.speed;
                    t["type"] = item.is_upload ? "upload" : "download";
                    t["total"] = item.total_chunks;
                    t["done"] = item.processed_chunks;
                    t["stalled"] = item.is_stalled;
                    node_json["transfers"].push_back(t);
                }

                // C. 获取对等点状态
                auto peers_info = p2p->get_peers_info();
                for (const auto& peer : peers_info) {
                    nlohmann::json peer_json;
                    peer_json["peer_id"] = peer.peer_id;
                    peer_json["state"] = peer.state;
                    peer_json["connection_type"] = peer.connection_type;
                    peer_json["connected_since"] = peer.connected_since;
                    peer_json["sync_key"] = node->get_key();
                    all_peers.push_back(peer_json);

                    if (peer.state == "connected") { connected_count++; node_connected_peers++; }
                    else if (peer.state == "connecting") connecting_count++;
                    else if (peer.state == "failed") failed_count++;
                }
            }

            // D. 推导任务运行状态
            bool tracker_online = node->is_tracker_online();
            std::string status = "stopped";
            if (node->is_started()) {
                if (has_active_transfers) {
                    status = "syncing";
                } else if (tracker_online && node_connected_peers > 0) {
                    status = "idle";
                } else if (tracker_online) {
                    status = "waiting";
                } else {
                    status = "offline";
                }
            }
            node_json["status"] = status;

            root["nodes"].push_back(node_json);
        }

        // 3. 构建全局状态对象
        root["global"] = {
            {"tracker_online", any_tracker_online}, 
            {"session_done", global_done}, 
            {"session_total", global_total}
        };
        
        // 4. 添加对等点信息 (新增)
        root["peers"] = all_peers;
        root["peers_summary"] = {
            {"total", all_peers.size()},
            {"connected", connected_count},
            {"connecting", connecting_count},
            {"failed", failed_count}
        };

        return root;
    });

    // --- 使用 std::jthread 启动 WebUI (C++20: 自动 join，支持 stop_token) ---
    std::promise<bool> webui_ready_promise;
    auto webui_ready_future = webui_ready_promise.get_future();
    bool webui_started = false;

    std::jthread ui_thread([&web_ui, &webui_ready_promise]([[maybe_unused]] std::stop_token stop_token) {
        // 辅助：安全地设置 promise 值（忽略重复 set_value 异常）
        auto safe_set = [&webui_ready_promise](bool val) {
            try { webui_ready_promise.set_value(val); } catch (...) {}
        };

        try {
            // 启动 WebUI 服务：通过回调将 bind 成功/失败结果回传给主线程
            web_ui.start([&safe_set](bool ok) { safe_set(ok); });
        } catch (const std::exception& e) {
            VeritasSync::g_logger->error("[Main] WebUI 线程异常: {}", e.what());
            safe_set(false);
        } catch (...) {
            VeritasSync::g_logger->error("[Main] WebUI 线程未知异常");
            safe_set(false);
        }

        // 注意：WebUI.start() 在监听成功后是阻塞的，当 web_ui.stop() 被调用后会返回
        // stop_token 可用于更细粒度的取消检查（如果将来 WebUI 内部支持）
    });

    // 等待 WebUI 启动握手结果，避免监听失败静默
    if (webui_ready_future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        VeritasSync::g_logger->error("[Main] WebUI 启动超时：无法确认监听状态");
        g_shutdown_requested = true;
        web_ui.stop();
        ui_thread.request_stop();
    } else {
        webui_started = webui_ready_future.get();
        if (!webui_started) {
            VeritasSync::g_logger->error("[Main] WebUI 启动失败（端口占用或监听失败）");
            g_shutdown_requested = true;
            web_ui.stop();
            ui_thread.request_stop();
        }
    }

#if defined(_WIN32)
    // --- 托盘图标逻辑 ---
    VeritasSync::TrayIcon tray;
    const bool tray_ready = tray.init("VeritasSync - P2P 同步节点");

    if (!tray_ready) {
        VeritasSync::g_logger->error("无法创建系统托盘图标，降级为控制台等待模式");
    } else {
        // 托盘就绪后覆盖重启回调：确保重启请求可唤醒 tray 消息循环退出
        web_ui.set_on_restart_request([&web_ui, &tray, &ui_thread]() {
            VeritasSync::g_logger->info("[Main] 收到 WebUI 重启请求，准备优雅退出...");
            g_restart_requested = true;
            g_shutdown_requested = true;
            web_ui.stop();
            ui_thread.request_stop();
            tray.quit();
        });
    }

    tray.add_menu_item("🌐 打开控制台", [&web_ui]() { VeritasSync::WebUIServer::open_url(web_ui.get_auth_url()); });

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

    tray.add_menu_item("🛑 退出程序", [&tray, &web_ui, &ui_thread]() {
        if (MessageBoxW(NULL, L"确定要退出同步服务吗？", L"VeritasSync", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            g_shutdown_requested = true;
            web_ui.stop();
            tray.quit();
            
            // 请求 jthread 停止（虽然 WebUI.stop() 已经会导致线程返回）
            ui_thread.request_stop();
        }
    });

    if (tray_ready) {
        if (!g_shutdown_requested) {
            VeritasSync::g_logger->info("系统托盘已启动。程序正在后台运行。");
            tray.run_loop();
        } else {
            VeritasSync::g_logger->info("检测到提前退出/重启请求，跳过托盘消息循环。");
        }
    } else {
        VeritasSync::g_logger->info("托盘不可用，使用控制台等待循环（可通过 WebUI/信号触发退出）");
        while (!g_shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#else
    if (webui_started) {
        VeritasSync::g_logger->info("\n--- 所有同步任务已启动。Web UI: http://127.0.0.1:{} | 按 Ctrl+C 退出 ---", config.webui.port);
    } else {
        VeritasSync::g_logger->warn("WebUI 未成功启动，程序将进入退出流程。");
    }

    // 非 Windows 平台：等待关闭信号 (SIGINT/SIGTERM 已注册)
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    // --- 清理阶段 ---
    VeritasSync::g_logger->info("正在停止所有服务...");
    
    // 停止 WebUI
    web_ui.stop();
    
    // std::jthread 析构时会自动 request_stop 和 join，无需手动调用
    
    // 统一关闭路径：由 SyncNode::stop() 内部执行 graceful shutdown
    {
        std::lock_guard<std::mutex> lock(g_nodes_mutex);
        for (const auto& node : g_active_nodes) {
            if (node) {
                node->stop();
            }
        }
        g_active_nodes.clear();
    }

#if defined(_WIN32)
    // 若不是重启，正常退出前也释放单实例句柄
    if (!g_restart_requested.load() && hMutex) {
        CloseHandle(hMutex);
        hMutex = nullptr;
    }
#endif

    if (g_restart_requested.load()) {
        VeritasSync::g_logger->info("检测到重启请求，正在拉起新实例...");

#if defined(_WIN32)
        // 【安全修复 C5】先释放 mutex，再用 CreateProcessW 同步创建新进程
        // CreateProcessW（相比原来的 ShellExecuteW）是同步的——返回时新进程已存在
        if (hMutex) {
            CloseHandle(hMutex);
            hMutex = nullptr;
        }
#endif

        if (!relaunch_current_process()) {
            VeritasSync::g_logger->error("重启失败：无法拉起新进程");
            VeritasSync::g_logger->info("--- Shutting down. ---");
            spdlog::shutdown();
            return 1;
        }
    }

    VeritasSync::g_logger->info("--- Shutting down. ---");
    spdlog::shutdown();
    return 0;
    } catch (const std::exception& e) {
        fatal_cleanup((std::string("[Main] 未捕获异常: ") + e.what()).c_str());
        return 2;
    } catch (...) {
        fatal_cleanup("[Main] 未捕获未知异常");
        return 3;
    }
}