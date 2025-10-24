#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "VeritasSync/P2PManager.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/TrackerClient.h"

// 引入 Boost.program_options 和 asio::signal_set
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>

// --- 不再需要 create_dummy_files ---

namespace bpo = boost::program_options;

int main(int argc, char* argv[]) {
  // --- 【修改】 1. 带异常处理的参数解析 (使用 boost::program_options) ---
  std::string sync_key;
  unsigned short p2p_port;
  std::string sync_folder;

  bpo::options_description desc("VeritasSync 命令行选项");
  desc.add_options()("help,h", "显示此帮助信息")(
      "key,k", bpo::value<std::string>(&sync_key)->required(),
      "同步密钥 (必需)")("port,p",
                         bpo::value<unsigned short>(&p2p_port)->required(),
                         "本地P2P端口 (必需)")(
      "folder,f", bpo::value<std::string>(&sync_folder)->required(),
      "要同步的文件夹 (必需)");

  bpo::variables_map vm;
  try {
    bpo::store(bpo::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help")) {
      std::cout << "--- Veritas Sync 节点 ---" << std::endl;
      std::cout << "一个 P2P 文件同步工具。" << std::endl;
      std::cout << desc << std::endl;
      std::cout << "示例: " << argv[0]
                << " -k my-secret-key -p 10001 -f ./SyncNode1" << std::endl;
      std::cout << "\nNOTE: Tracker 服务器必须运行在 localhost:9988."
                << std::endl;
      return 0;
    }

    // 检查所有必需的选项
    bpo::notify(vm);

  } catch (const bpo::error& e) {
    std::cerr << "[Error] 参数解析失败: " << e.what() << std::endl;
    std::cerr << desc << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "[Error] " << e.what() << std::endl;
    return 1;
  }

  std::cout << "--- Veritas Sync 节点 ---" << std::endl;
  std::cout << "[Config] Sync Key: " << sync_key << std::endl;
  std::cout << "[Config] P2P Port: " << p2p_port << std::endl;
  std::cout << "[Config] Sync Folder: " << sync_folder << std::endl;

  // --- 【移除】 删除了 if (exists) / remove_all / create_dummy_files ---
  // 我们现在操作的是真实的用户目录

  // 1. 创建 P2PManager
  auto p2p_manager = VeritasSync::P2PManager::create(p2p_port);

  // 2. 创建 StateManager，并将p2p_manager的引用传递给它
  VeritasSync::StateManager state_manager(sync_folder, *p2p_manager);

  // 3. 将 StateManager 的地址“注入”到 P2PManager 中
  p2p_manager->set_state_manager(&state_manager);

  // --- 【移除】 删除了第二次 create_dummy_files 调用 ---

  // 4. 初始化文件并进行一次初始扫描和广播
  std::cout << "[Init] 正在执行初始目录扫描..." << std::endl;
  state_manager.scan_directory();
  p2p_manager->broadcast_current_state();

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

  // --- 【修改】 5. 设置优雅关闭 (Ctrl+C) ---
  // 替换 std::this_thread::sleep_for

  std::cout << "\n--- Node is running. Press Ctrl+C to exit. ---" << std::endl;

  // 创建一个 signal_set 来监听 SIGINT (Ctrl+C) 和 SIGTERM
  // 我们在 p2p_manager 的 io_context 上运行它
  boost::asio::signal_set signals(p2p_manager->get_io_context(), SIGINT,
                                  SIGTERM);

  signals.async_wait([&p2p_manager](const boost::system::error_code&, int) {
    std::cout << "\n[Signal] 收到关闭信号 (Ctrl+C)。正在停止..." << std::endl;
    // 停止 io_context。这将导致 p2p_manager 的 m_thread 结束。
    p2p_manager->get_io_context().stop();
  });

  // main 线程现在在此处阻塞，并负责运行所有异步网络 IO
  p2p_manager->get_io_context().run();

  // (当 io_context 被 stop() 后，run() 会返回，程序继续执行到这里)

  std::cout << "[Shutdown] 节点已停止。" << std::endl;

  return 0;
}