#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>  // for std::unique_ptr
#include <stdexcept>
#include <string>
#include <thread>

// --- 正确的头文件顺序 ---
// 1. 日志 和 配置 (新增)
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"

// 2. 项目核心头文件
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN  // 避免 winsock 冲突
#include <windows.h>
#endif
// ------------------------------------------

// --- 步骤 4.A: 定义全局 logger ---
namespace VeritasSync {
std::shared_ptr<spdlog::logger> g_logger;
}

// --- 步骤 4.A: 初始化日志 ---
void init_logger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);  // 控制台级别

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("veritas_sync.log", 1024 * 1024 * 5,
                                                                                3);  // 5MB, 3个文件
        file_sink->set_level(spdlog::level::debug);                                  // 文件级别

        // 初始化异步日志线程池
        spdlog::init_thread_pool(8192, 1);

        VeritasSync::g_logger =
            std::make_shared<spdlog::async_logger>("veritas_sync", spdlog::sinks_init_list{console_sink, file_sink},
                                                   spdlog::thread_pool(), spdlog::async_overflow_policy::block);

        VeritasSync::g_logger->set_level(spdlog::level::debug);  // 设置总级别
        VeritasSync::g_logger->flush_on(spdlog::level::info);    // 遇到 info 及以上级别立即刷盘
        spdlog::register_logger(VeritasSync::g_logger);
        spdlog::set_default_logger(VeritasSync::g_logger);

    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        exit(1);
    }
}

// (create_dummy_files 函数保持不变)
void create_dummy_files(const std::string& dir, const std::string& node_id) {
    std::filesystem::path root(dir);
    std::filesystem::create_directories(root);

    if (node_id == "node1") {
        VeritasSync::g_logger->info("[TestSetup] (Source) Creating files for Node 1...");
        std::ofstream(root / "file_from_node1.txt") << "This file originated on Node 1.";
        std::filesystem::create_directory(root / "common_dir");
        std::ofstream(root / "common_dir" / "doc_A.txt") << "Document A";
    } else if (node_id == "node2") {
        VeritasSync::g_logger->info("[TestSetup] (Source) Creating files for Node 2...");
        std::ofstream(root / "log_from_node2.log") << "This log file originated on Node 2.";
        std::filesystem::create_directory(root / "common_dir");
        std::ofstream(root / "common_dir" / "doc_B.txt") << "Document B";
    }
}

// --- 步骤 4.B: 封装 "Node" 类 (管理单个同步任务) ---
class SyncNode {
   public:
    SyncNode(VeritasSync::SyncTask task, const std::string& tracker_host, unsigned short tracker_port)
        : m_task(std::move(task)), m_tracker_host(tracker_host), m_tracker_port(tracker_port) {}

    // 启动此同步任务
    void start() {
        VeritasSync::g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
        VeritasSync::g_logger->info("[Config] Role: {}", m_task.role);
        VeritasSync::g_logger->info("[Config] P2P Port: {}", m_task.p2p_port);
        VeritasSync::g_logger->info("[Config] Sync Folder: {}", m_task.sync_folder);

        VeritasSync::SyncRole role;
        bool is_source;
        if (m_task.role == "source") {
            role = VeritasSync::SyncRole::Source;
            is_source = true;
        } else if (m_task.role == "destination") {
            role = VeritasSync::SyncRole::Destination;
            is_source = false;
        } else {
            VeritasSync::g_logger->error("Invalid role: '{}' for task '{}'. Must be 'source' or 'destination'.",
                                         m_task.role, m_task.sync_key);
            return;
        }

        // (设置文件夹 - 逻辑从旧 main 迁移)
        if (is_source) {
            if (std::filesystem::exists(m_task.sync_folder)) {
                std::filesystem::remove_all(m_task.sync_folder);
            }
            // --- 修复 BUG 2: 检查 "SyncNode_A" 而不是 "Node1" ---
            if (m_task.sync_folder.find("SyncNode_A") != std::string::npos) {
                create_dummy_files(m_task.sync_folder, "node1");
            } else if (m_task.sync_folder.find("SyncNode_B") != std::string::npos) {
                // (为 config.json 中的 "node2" 示例保留)
                create_dummy_files(m_task.sync_folder, "node2");
            } else {
                std::filesystem::create_directories(m_task.sync_folder);
                VeritasSync::g_logger->info("[TestSetup] (Source) Using empty directory.");
            }
        } else {
            if (!std::filesystem::exists(m_task.sync_folder)) {
                std::filesystem::create_directories(m_task.sync_folder);
                VeritasSync::g_logger->info("[TestSetup] (Destination) Folder created.");
            }
        }

        // 1. 创建 P2PManager
        m_p2p_manager = VeritasSync::P2PManager::create(m_task.p2p_port);
        m_p2p_manager->set_role(role);
        m_p2p_manager->set_encryption_key(m_task.sync_key);

        // 2. 创建 StateManager
        m_state_manager = std::make_unique<VeritasSync::StateManager>(m_task.sync_folder, *m_p2p_manager, is_source);

        // 3. 注入 StateManager
        m_p2p_manager->set_state_manager(m_state_manager.get());

        // 4. 初始扫描
        m_state_manager->scan_directory();

        // 5. Source 启动时广播
        if (is_source) {
            m_p2p_manager->broadcast_current_state();
        }

        VeritasSync::g_logger->info("[{}] --- Phase 1: Contacting Tracker ---", m_task.sync_key);
        VeritasSync::TrackerClient tracker_client(m_tracker_host, m_tracker_port);
        std::vector<std::string> peer_addresses = tracker_client.register_and_query(m_task.sync_key, m_task.p2p_port);

        VeritasSync::g_logger->info("[{}] Received {} peer(s) from tracker.", m_task.sync_key, peer_addresses.size());
        for (const auto& peer : peer_addresses) {
            VeritasSync::g_logger->info("  - Peer: {}", peer);
        }

        VeritasSync::g_logger->info("[{}] --- Phase 2: P2P State Synchronization ---", m_task.sync_key);
        if (!peer_addresses.empty()) {
            m_p2p_manager->connect_to_peers(peer_addresses);
        } else {
            VeritasSync::g_logger->info("[{}] No other peers in the group. Waiting for others to join.",
                                        m_task.sync_key);
        }
    }

   private:
    VeritasSync::SyncTask m_task;
    std::string m_tracker_host;
    unsigned short m_tracker_port;

    // 必须持有这些对象，否则它们将被销毁
    std::shared_ptr<VeritasSync::P2PManager> m_p2p_manager;
    std::unique_ptr<VeritasSync::StateManager> m_state_manager;
};

int main(int argc, char* argv[]) {
    // --- 设置控制台编码 ---
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --- 步骤 4.A: 初始化日志 ---
    init_logger();
    VeritasSync::g_logger->info("--- Veritas Sync Node Starting Up ---");

#if defined(_WIN32)
    VeritasSync::g_logger->info("[System] Windows console output set to UTF-8.");
#endif

    // --- 步骤 4.B: 加载配置 ---
    VeritasSync::Config config;
    try {
        // --- 修复 BUG 1: 添加 try...catch 块 ---
        config = VeritasSync::load_config_or_create_default("config.json");
    } catch (const std::exception& e) {
        // 这是预期的行为，当 config.json 第一次被创建时
        VeritasSync::g_logger->warn("{}", e.what());
        VeritasSync::g_logger->info("Please edit config.json and restart. Shutting down.");
        spdlog::shutdown();  // 确保异步日志在退出前刷盘
        return 1;
    }

    VeritasSync::g_logger->info("Configuration loaded. Tracker at {}:{}. Found {} task(s).", config.tracker_host,
                                config.tracker_port, config.tasks.size());

    if (config.tasks.empty()) {
        VeritasSync::g_logger->warn("No sync tasks defined in config.json. Exiting.");
        spdlog::shutdown();
        return 0;
    }

    // --- 步骤 4.B: 启动所有任务 ---
    std::vector<std::unique_ptr<SyncNode>> nodes;
    for (const auto& task : config.tasks) {
        auto node = std::make_unique<SyncNode>(task, config.tracker_host, config.tracker_port);
        node->start();
        nodes.push_back(std::move(node));  // 保存 node 以防被销毁
    }

    VeritasSync::g_logger->info("\n--- All nodes are running. Press Ctrl+C to exit. ---");

    // --- 修复 BUG 2 (我的拼写错误):
    // 将 -> 替换为 ::
    std::this_thread::sleep_for(std::chrono::hours(24));

    VeritasSync::g_logger->info("--- Shutting down. ---");

    spdlog::shutdown();  // 程序正常退出时也刷盘
    return 0;
}
