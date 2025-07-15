#include <iostream>
#include <thread>

#include "VeritasSync/P2PManager.h"
#include "VeritasSync/TrackerClient.h"

int main(int argc, char* argv[]) {
  try {
    std::cout << "--- Veritas Sync Peer ---" << std::endl;

    // 1. 获取我们自己的P2P端口
    unsigned short my_port = 9001;
    if (argc > 1) {
      my_port = static_cast<unsigned short>(std::stoul(argv[1]));
    }
    std::cout << "[Main] Our P2P port is " << my_port << std::endl;

    // 2. 连接Tracker并获取节点列表
    VeritasSync::TrackerClient tracker_client("127.0.0.1", 9988);
    std::string sync_key = "my-secret-room";
    std::vector<std::string> peers =
        tracker_client.register_and_query(sync_key, my_port);
    std::cout << "[Main] Found " << peers.size()
              << " other peer(s) from tracker." << std::endl;

    // 3. 初始化P2P管理器
    VeritasSync::P2PManager p2p_manager(my_port);

    // 4. 尝试向所有发现的节点发送“打洞”消息
    if (!peers.empty()) {
      p2p_manager.connect_to_peers(peers);
    }

    // 5. 让主程序持续运行，以便后台线程能处理P2P消息
    std::cout << "[Main] P2P Manager is running. Press Enter to exit."
              << std::endl;
    std::cin.get();

  } catch (const std::exception& e) {
    std::cerr << "Exception in main: " << e.what() << std::endl;
  }

  return 0;
}