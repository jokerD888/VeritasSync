#include "VeritasSync/StateManager.h"
#include "VeritasSync/Hashing.h"
#include "VeritasSync/P2PManager.h" 
#include <iostream>
#include <efsw/efsw.hpp>


#include <boost/asio.hpp>

namespace VeritasSync {



// 这个类继承自 efsw::FileWatchListener，用于接收文件变化通知
class UpdateListener : public efsw::FileWatchListener {
 public:
  UpdateListener(P2PManager& p2p_manager)
      : m_p2p_manager(p2p_manager),
        m_debounce_timer(p2p_manager.get_io_context()) {}

  void handleFileAction(efsw::WatchID, const std::string& dir,
                        const std::string& filename, efsw::Action action,
                        std::string) override {
    if (filename == "." || filename == "..") {
      return;
    }

    // 日志打印逻辑不变
    std::cout << "[Watcher] 检测到变化: " << dir + filename << std::endl;

    m_debounce_timer.cancel();
    m_debounce_timer.expires_after(std::chrono::milliseconds(200));

    // 直接捕获 [this]，因为 StateManager 保证了我们的生命周期
    m_debounce_timer.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) {
        std::cout << "[Watcher] 文件系统稳定，触发状态广播。" << std::endl;
        boost::asio::post(m_p2p_manager.get_io_context(), [this]() {
          m_p2p_manager.broadcast_current_state();
        });
      }
    });
  }

 private:
  P2PManager& m_p2p_manager;
  boost::asio::steady_timer m_debounce_timer;
};

  StateManager::StateManager(const std::string& root_path,
                            P2PManager& p2p_manager)
    : m_root_path(root_path) {
  if (!std::filesystem::exists(m_root_path)) {
    std::cout << "[StateManager] 根目录 " << m_root_path
              << " 不存在，正在创建。" << std::endl;
    std::filesystem::create_directory(m_root_path);
  }

  m_file_watcher = std::make_unique<efsw::FileWatcher>();

  // 直接创建 unique_ptr 实例
  m_listener = std::make_unique<UpdateListener>(p2p_manager);

  m_file_watcher->addWatch(m_root_path.string(), m_listener.get(), true);
  m_file_watcher->watch();
  std::cout << "[StateManager] 已启动对目录 '" << m_root_path.string()
            << "' 的实时监控。" << std::endl;
  }

  StateManager::~StateManager() {
  std::cout << "[StateManager] 正在停止文件监控..." << std::endl;
  // unique_ptr 会自动处理销毁，无需代码
  }

  void StateManager::scan_directory() {
    std::cout << "[StateManager] Scanning directory: " << m_root_path << std::endl;
    m_file_map.clear();

    // --- 关键修改 ---
    // 为迭代器构造函数提供一个error_code对象，以避免在路径出错时抛出异常
    std::error_code ec;
    auto iterator = std::filesystem::recursive_directory_iterator(m_root_path, ec);

    if (ec) {
      std::cerr << "[StateManager] Error creating directory iterator for path "
        << m_root_path << ": " << ec.message() << std::endl;
      return; // 如果创建迭代器失败，则直接返回
    }

    for (const auto& entry : iterator) {
      // 在循环内部也进行检查，确保entry本身是有效的
      if (!entry.is_regular_file(ec) || ec) {
        // 如果不是常规文件或检查时出错，跳过这个条目
        continue;
      }

      FileInfo info;

        // 1. 获取相对路径
      std::filesystem::path relative_path =
          std::filesystem::relative(entry.path(), m_root_path);

      // 2. 使用 u8string() 方法将路径安全地转换为UTF-8编码的字符串
      //    这可以正确处理包括中文在内的所有Unicode字符。
      const std::u8string u8_path_str = relative_path.u8string();
      info.path =
          std::string(reinterpret_cast<const char*>(u8_path_str.c_str()),
                      u8_path_str.length());

      // 2. 获取最后修改时间 (这部分逻辑不变)
      auto ftime = std::filesystem::last_write_time(entry, ec);
      if (ec) continue;
      auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
      info.modified_time = sctp.time_since_epoch().count();

      // 3. 计算哈希值 (这部分逻辑不变)
      info.hash = Hashing::CalculateSHA256(entry.path());
      if (info.hash.empty()) continue;

      // 4. 存入map
      m_file_map[info.path] = info;
    }
    std::cout << "[StateManager] Scan complete. Found " << m_file_map.size() << " files." << std::endl;
  }

  std::string StateManager::get_state_as_json_string() {
    // 1. 将map中的FileInfo对象转换成一个vector
    std::vector<FileInfo> files;
    for (const auto& pair : m_file_map) {
      files.push_back(pair.second);
    }

    // 2. 使用nlohmann/json库构建JSON对象
    nlohmann::json payload;
    payload["files"] = files; // 这里利用了我们之前定义的 to_json 函数

    nlohmann::json message;
    message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
    message[Protocol::MSG_PAYLOAD] = payload;

    // 3. 将JSON对象序列化为字符串
    return message.dump(2); // dump(2) 表示使用2个空格进行格式化，便于阅读
  }

  void StateManager::print_current_state() const {
    std::cout << "\n--- Current Directory State ---" << std::endl;
    for (const auto& pair : m_file_map) {
      std::cout << "  - Path: " << pair.second.path << std::endl;
      std::cout << "    MTime: " << pair.second.modified_time << std::endl;
      std::cout << "    Hash: " << pair.second.hash.substr(0, 12) << "..." << std::endl;
    }
    std::cout << "-----------------------------" << std::endl;
  }
}