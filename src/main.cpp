#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "VeritasSync/Config.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace VeritasSync {
std::shared_ptr<spdlog::logger> g_logger;
}

// (init_logger ... 保持不变 ...)
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

// (create_dummy_files ... 保持不变 ...)
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

class SyncNode {
   public:
    SyncNode(VeritasSync::SyncTask task, const VeritasSync::Config& global_config)
        : m_task(std::move(task)), m_global_config(global_config) {}

    void start() {
        VeritasSync::g_logger->info("--- Starting Sync Task [{}] ---", m_task.sync_key);
        VeritasSync::g_logger->info("[Config] Role: {}", m_task.role);
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
            VeritasSync::g_logger->error("Invalid role: '{}' for task '{}'.", m_task.role, m_task.sync_key);
            return;
        }

        if (is_source) {
            if (std::filesystem::exists(m_task.sync_folder)) {
                std::filesystem::remove_all(m_task.sync_folder);
            }
            if (m_task.sync_folder.find("SyncNode_A") != std::string::npos) {
                create_dummy_files(m_task.sync_folder, "node1");
            } else if (m_task.sync_folder.find("SyncNode_B") != std::string::npos) {
                create_dummy_files(m_task.sync_folder, "node2");
            } else {
                std::filesystem::create_directories(m_task.sync_folder);
                // --- 修复 1：添加命名空间 ---
                VeritasSync::g_logger->info("[TestSetup] (Source) Using empty directory.");
            }
        } else {
            if (!std::filesystem::exists(m_task.sync_folder)) {
                std::filesystem::create_directories(m_task.sync_folder);
                // --- 修复 2：添加命名空间 ---
                VeritasSync::g_logger->info("[TestSetup] (Destination) Folder created.");
            }
        }

        // --- 修复：启动顺序 ---

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
        if (!m_global_config.turn_host.empty()) {
            VeritasSync::g_logger->info("[Config] Using TURN server at {}:{}", m_global_config.turn_host,
                                        m_global_config.turn_port);
            m_p2p_manager->set_turn_config(m_global_config.turn_host, m_global_config.turn_port,
                                           m_global_config.turn_username, m_global_config.turn_password);
        } else {
            VeritasSync::g_logger->info("[Config] No TURN server configured.");
        }

        // 5. 创建 StateManager
        m_state_manager = std::make_unique<VeritasSync::StateManager>(m_task.sync_folder, *m_p2p_manager, is_source);

        // 6. 注入 StateManager
        m_p2p_manager->set_state_manager(m_state_manager.get());

        // 7. 初始扫描
        m_state_manager->scan_directory();

        // 9. --- 启动信令连接 ---
        VeritasSync::g_logger->info("[{}] --- Phase 1: Contacting Tracker ---", m_task.sync_key);

        // --- 修复 3：此调用现在与 TrackerClient.h 中的新签名匹配 ---
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
    // (main 函数 ... 保持不变 ...)
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    init_logger();
    VeritasSync::g_logger->info("--- Veritas Sync Node Starting Up ---");
#if defined(_WIN32)
    VeritasSync::g_logger->info("[System] Windows console output set to UTF-8.");
#endif
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
    if (config.tasks.empty()) {
        VeritasSync::g_logger->warn("No sync tasks defined in config.json. Exiting.");
        spdlog::shutdown();
        return 0;
    }
    std::vector<std::unique_ptr<SyncNode>> nodes;
    for (const auto& task : config.tasks) {
        auto node = std::make_unique<SyncNode>(task, config);
        node->start();
        nodes.push_back(std::move(node));
    }
    VeritasSync::g_logger->info("\n--- All nodes are running. Press Ctrl+C to exit. ---");
    std::this_thread::sleep_for(std::chrono::hours(24));
    VeritasSync::g_logger->info("--- Shutting down. ---");
    spdlog::shutdown();
    return 0;
}
