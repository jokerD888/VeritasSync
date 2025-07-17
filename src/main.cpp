#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

// --- 函数已修改 ---
// 现在它会根据同步目录名创建不同的文件，以方便测试
void create_dummy_files(const std::string& dir) {
  std::filesystem::path root(dir);
  std::filesystem::create_directories(root);  // 确保根目录存在

  if (dir.find("SyncNode1") != std::string::npos) {
    std::ofstream(root / "file1.txt") << "This is a file unique to Node 1.";
    std::ofstream(root / "shared.txt")
        << "This is the original version of the shared file.";
  } else if (dir.find("SyncNode2") != std::string::npos) {
    std::ofstream(root / "file2.log") << "This is a log file unique to Node 2.";
    std::ofstream(root / "shared.txt")
        << "This is a MODIFIED version of the shared file on Node 2.";
  } else {
    // 默认情况
    std::ofstream(root / "default.txt") << "default content";
  }
}

int main(int argc, char* argv[]) {
  // ... main函数的其余部分保持不变 ...
  // --- 参数检查 ---
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

  VeritasSync::StateManager state_manager(sync_folder);
  VeritasSync::P2PManager p2p_manager(p2p_port, state_manager);

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
    p2p_manager.connect_to_peers(peer_addresses);
  } else {
    std::cout
        << "[P2P] No other peers in the group. Waiting for others to join."
        << std::endl;
  }

  std::cout << "\n--- Node is running. Press Ctrl+C to exit. ---" << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(20));
  std::cout << "--- Test finished. Shutting down. ---" << std::endl;

  return 0;
}