#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

// ---：创建不冲突的文件集 ---
void create_dummy_files(const std::string& dir) {
  std::filesystem::path root(dir);
  std::filesystem::create_directories(root);

  if (dir.find("SyncNode1") != std::string::npos) {
    std::cout << "[TestSetup] Creating files for Node 1..." << std::endl;
    std::ofstream(root / "file_from_node1.txt") << "This file originated on Node 1.";
    std::filesystem::create_directory(root / "common_dir");
    std::ofstream(root / "common_dir" / "doc_A.txt") << "Document A";
  }
  else if (dir.find("SyncNode2") != std::string::npos) {
    std::cout << "[TestSetup] Creating files for Node 2..." << std::endl;
    std::ofstream(root / "log_from_node2.log") << "This log file originated on Node 2.";
    std::filesystem::create_directory(root / "common_dir");
    std::ofstream(root / "common_dir" / "doc_B.txt") << "Document B";
  }
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " <sync_key> <p2p_port> <sync_folder>"
              << std::endl;
    std::cerr << "Example: " << argv[0] << " my-secret-key 10001 ./SyncNode1"
              << std::endl;
    std::cerr << "\nNOTE: The Tracker server must be running on localhost:9988."
              << std::endl;
    return 1;
  }

  // --- 带异常处理的参数解析 ---
  std::string sync_key = argv[1];
  unsigned short p2p_port = 0;
  std::string sync_folder = argv[3];

  try {
    p2p_port = static_cast<unsigned short>(std::stoi(argv[2]));
    if (p2p_port == 0) {
      throw std::invalid_argument("Port number cannot be 0.");
    }
  } catch (const std::invalid_argument& e) {
    std::cerr << "[Error] Invalid P2P port provided. '" << argv[2]
              << "' is not a valid number. Details: " << e.what() << std::endl;
    return 1;
  } catch (const std::out_of_range& e) {
    std::cerr << "[Error] P2P port provided '" << argv[2]
              << "' is out of range for a port number. Details: " << e.what()
              << std::endl;
    return 1;
  }

  std::cout << "--- Veritas Sync Node ---" << std::endl;
  std::cout << "[Config] Sync Key: " << sync_key << std::endl;
  std::cout << "[Config] P2P Port: " << p2p_port << std::endl;
  std::cout << "[Config] Sync Folder: " << sync_folder << std::endl;

  if (std::filesystem::exists(sync_folder)) {
    std::filesystem::remove_all(sync_folder);
  }
  create_dummy_files(sync_folder);
  std::cout << "[TestSetup] Dummy files created in " << sync_folder
            << std::endl;
  // 1. 创建 P2PManager
  auto p2p_manager = VeritasSync::P2PManager::create(p2p_port);

  // 2. 创建 StateManager，并将p2p_manager的引用传递给它
  VeritasSync::StateManager state_manager(sync_folder, *p2p_manager);

  // 3. 将 StateManager 的地址“注入”到 P2PManager 中
  p2p_manager->set_state_manager(&state_manager);

  // 初始化文件并进行一次初始扫描和广播
  create_dummy_files(sync_folder);
  std::cout << "[TestSetup] Dummy files created in " << sync_folder
            << std::endl;
  state_manager.scan_directory();
  p2p_manager->broadcast_current_state();  // 可选：启动时广播一次状态

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
  std::this_thread::sleep_for(std::chrono::seconds(100));
  std::cout << "--- Test finished. Shutting down. ---" << std::endl;

  return 0;
}