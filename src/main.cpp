#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

// --- 正确的头文件顺序，用于修复编译和乱码 ---
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN  // 避免 winsock 冲突
#include <windows.h>
#endif
// ------------------------------------------

// ---：创建不冲突的文件集 ---
void create_dummy_files(const std::string& dir, const std::string& node_id) {
  std::filesystem::path root(dir);
  std::filesystem::create_directories(root);

  if (node_id == "node1") {
    std::cout << "[TestSetup] (Source) Creating files for Node 1..."
              << std::endl;
    std::ofstream(root / "file_from_node1.txt")
        << "This file originated on Node 1.";
    std::filesystem::create_directory(root / "common_dir");
    std::ofstream(root / "common_dir" / "doc_A.txt") << "Document A";
  } else if (node_id == "node2") {
    std::cout << "[TestSetup] (Source) Creating files for Node 2..."
              << std::endl;
    std::ofstream(root / "log_from_node2.log")
        << "This log file originated on Node 2.";
    std::filesystem::create_directory(root / "common_dir");
    std::ofstream(root / "common_dir" / "doc_B.txt") << "Document B";
  }
}

int main(int argc, char* argv[]) {
  // --- 设置控制台编码 ---
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
  std::cout << "[System] Windows console output set to UTF-8." << std::endl;
#endif

  // --- 参数检查 ---
  if (argc != 5) {  // 需要 5 个参数
    std::cerr << "Usage: " << argv[0]
              << " <mode> <sync_key> <p2p_port> <sync_folder>" << std::endl;
    std::cerr << "Mode: 'source' 或 'destination'" << std::endl;
    std::cerr << "Example (Source): " << argv[0]
              << " source my-key 10001 ./SyncNode_A" << std::endl;
    std::cerr << "Example (Dest):   " << argv[0]
              << " destination my-key 10002 ./SyncNode_B" << std::endl;
    std::cerr << "\nNOTE: The Tracker server must be running on localhost:9988."
              << std::endl;
    return 1;
  }

  // --- 解析参数 ---
  std::string mode_str = argv[1];
  std::string sync_key = argv[2];
  unsigned short p2p_port = 0;
  std::string sync_folder = argv[4];

  VeritasSync::SyncRole role;
  bool is_source;
  if (mode_str == "source") {
    role = VeritasSync::SyncRole::Source;
    is_source = true;
  } else if (mode_str == "destination") {
    role = VeritasSync::SyncRole::Destination;
    is_source = false;
  } else {
    std::cerr << "[Error] Invalid mode: '" << mode_str
              << "'. Must be 'source' or 'destination'." << std::endl;
    return 1;
  }

  try {
    p2p_port = static_cast<unsigned short>(std::stoi(argv[3]));
    if (p2p_port == 0) {
      throw std::invalid_argument("Port number cannot be 0.");
    }
  } catch (const std::exception& e) {
    std::cerr << "[Error] Invalid P2P port provided. '" << argv[3] << "'. "
              << e.what() << std::endl;
    return 1;
  }

  std::cout << "--- Veritas Sync Node ---" << std::endl;
  std::cout << "[Config] Mode: " << mode_str << std::endl;
  std::cout << "[Config] Sync Key: " << sync_key << std::endl;
  std::cout << "[Config] P2P Port: " << p2p_port << std::endl;
  std::cout << "[Config] Sync Folder: " << sync_folder << std::endl;

  if (is_source) {
    if (std::filesystem::exists(sync_folder)) {
      std::filesystem::remove_all(sync_folder);
    }
    if (sync_folder.find("Node1") != std::string::npos) {
      create_dummy_files(sync_folder, "node1");
    } else if (sync_folder.find("Node2") != std::string::npos) {
      create_dummy_files(sync_folder, "node2");
    } else {
      std::filesystem::create_directories(sync_folder);
      std::cout << "[TestSetup] (Source) 正在使用空目录。" << std::endl;
    }
  } else {
    if (!std::filesystem::exists(sync_folder)) {
      std::filesystem::create_directories(sync_folder);
      std::cout << "[TestSetup] (Destination) 文件夹已创建。" << std::endl;
    }
  }

  // 1. 创建 P2PManager
  auto p2p_manager = VeritasSync::P2PManager::create(p2p_port);

  // 2. 注入角色
  p2p_manager->set_role(role);

  // (移除 set_encryption_key)

  // 3. 创建 StateManager
  VeritasSync::StateManager state_manager(sync_folder, *p2p_manager, is_source);

  // 4. 注入 StateManager
  p2p_manager->set_state_manager(&state_manager);

  // 5. 初始扫描
  state_manager.scan_directory();

  // 6. Source 启动时广播
  if (is_source) {
    p2p_manager->broadcast_current_state();
  }

  std::cout << "\n--- Phase 1: Contacting Tracker ---" << std::endl;
  VeritasSync::TrackerClient tracker_client("127.0.0.1", 9988);
  std::vector<std::string> peer_addresses =
      tracker_client.register_and_query(sync_key, p2p_port);

  std::cout << "[Tracker] Received " << peer_addresses.size()
            << " peer(s) from tracker." << std::endl;
  for (const auto& peer : peer_addresses) {
    std::cout << "  - Peer: " << peer << std::endl;
  }

  std::cout << "\n--- Phase 2: P2P State Synchronization ---" << std::endl;
  if (!peer_addresses.empty()) {
    p2p_manager->connect_to_peers(peer_addresses);
  } else {
    std::cout
        << "[P2P] No other peers in the group. Waiting for others to join."
        << std::endl;
  }

  std::cout << "\n--- Node is running. Press Ctrl+C to exit. ---" << std::endl;
  std::this_thread::sleep_for(std::chrono::hours(24));
  std::cout << "--- Shutting down. ---" << std::endl;

  return 0;
}
