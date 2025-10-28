#include "VeritasSync/StateManager.h"
#include "VeritasSync/Hashing.h"
#include "VeritasSync/P2PManager.h"

#include <boost/asio.hpp>
#include <efsw/efsw.hpp>
#include <iostream>


namespace VeritasSync {

// 这个类继承自 efsw::FileWatchListener，用于接收文件变化通知
class UpdateListener : public efsw::FileWatchListener {
 public:
  // --- 修改：构造函数现在接收 StateManager* ---
  UpdateListener(StateManager* owner)
      : m_owner(owner),
        m_debounce_timer(owner->get_io_context()) {
  }  // 从 owner 获取 io_context

  void handleFileAction(efsw::WatchID, const std::string& dir,
                        const std::string& filename, efsw::Action action,
                        std::string) override {
    if (filename == "." || filename == "..") {
      return;
    }

    // 1. 通知 StateManager 将变化暂存
    std::filesystem::path dir_path(dir);
    std::filesystem::path file_path = dir_path / filename;
    m_owner->notify_change_detected(file_path.generic_string());

    // 2. 重置防抖计时器
    m_debounce_timer.cancel();
    m_debounce_timer.expires_after(std::chrono::milliseconds(200));

    // 3. 计时器触发后，调用 StateManager 的处理函数
    m_debounce_timer.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) {
        // 使用 post 确保在 io_context 线程上执行
        boost::asio::post(m_owner->get_io_context(),
                          [this]() { m_owner->process_debounced_changes(); });
      }
    });
  }

 private:
  StateManager* m_owner;  // --- 修改：持有 StateManager 指针 ---
  boost::asio::steady_timer m_debounce_timer;
};

StateManager::StateManager(const std::string& root_path,
                           P2PManager& p2p_manager, bool enable_watcher)
    : m_root_path(root_path), m_p2p_manager(&p2p_manager) {
  if (!std::filesystem::exists(m_root_path)) {
    std::cout << "[StateManager] 根目录 " << m_root_path
              << " 不存在，正在创建。" << std::endl;
    std::filesystem::create_directory(m_root_path);
  }

if (enable_watcher) {
    m_file_watcher = std::make_unique<efsw::FileWatcher>();
    // --- 修改：将 'this' 传递给 UpdateListener ---
    m_listener = std::make_unique<UpdateListener>(this);
    m_file_watcher->addWatch(m_root_path.string(), m_listener.get(), true);
    m_file_watcher->watch();
    std::cout << "[StateManager] 已启动对目录 '" << m_root_path.string()
              << "' 的实时监控 (Source 模式)。" << std::endl;
  } else {
    std::cout << "[StateManager] 以 'Destination' 模式启动，文件监控已禁用。"
              << std::endl;
  }
}

StateManager::~StateManager() {
  if (m_file_watcher) {
    std::cout << "[StateManager] 正在停止文件监控..." << std::endl;
  }
  // unique_ptr 会自动处理销毁，无需代码
}

boost::asio::io_context& StateManager::get_io_context() {
  return m_p2p_manager->get_io_context();
}

// --- 新增：实现 notify_change_detected (由 UpdateListener 调用) ---
void StateManager::notify_change_detected(const std::string& full_path) {
  std::lock_guard<std::mutex> lock(m_changes_mutex);
  m_pending_changes.insert(full_path);
  std::cout << "[Watcher] 检测到变化: " << full_path << std::endl;
}

// --- 新增：实现 process_debounced_changes (由 UpdateListener 定时器调用) ---
void StateManager::process_debounced_changes() {
  std::cout << "[Watcher] 文件系统稳定，正在处理增量变化..." << std::endl;

  std::set<std::string> changes_to_process;
  {
    std::lock_guard<std::mutex> lock(m_changes_mutex);
    // 交换数据，快速释放锁
    m_pending_changes.swap(changes_to_process);
  }

  std::lock_guard<std::mutex> map_lock(m_file_map_mutex);  // 锁定 m_file_map

  for (const auto& full_path_str : changes_to_process) {
    std::filesystem::path full_path(full_path_str);
    std::filesystem::path relative_path;

    // efsw 可能会在根目录创建/删除时给出根目录本身的路径
    if (full_path == m_root_path) continue;

    std::error_code ec;
    relative_path = std::filesystem::relative(full_path, m_root_path, ec);
    if (ec) continue;

    // 将 std::filesystem::path 安全地转为 UTF-8 字符串
    const std::u8string u8_path_str = relative_path.u8string();
    std::string rel_path_str(reinterpret_cast<const char*>(u8_path_str.c_str()),
                             u8_path_str.length());

    // --- 判断是“更新”还是“删除” ---
    if (std::filesystem::exists(full_path, ec) && !ec) {
      // 文件存在 (Add 或 Modified)
      if (!std::filesystem::is_regular_file(full_path, ec) || ec) {
        // 如果是目录或其他，则跳过
        continue;
      }

      FileInfo info;
      info.path = rel_path_str;

      auto ftime = std::filesystem::last_write_time(full_path, ec);
      if (ec) continue;
      auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
      info.modified_time = sctp.time_since_epoch().count();

      info.hash = Hashing::CalculateSHA256(full_path);
      if (info.hash.empty()) continue;

      // 更新 m_file_map 并广播
      m_file_map[info.path] = info;
      std::cout << "[StateManager] 广播更新: " << info.path << std::endl;
      m_p2p_manager->broadcast_file_update(info);

    } else {
      // 文件不存在 (Delete)
      if (m_file_map.erase(rel_path_str) > 0) {
        // 仅当文件之前在 map 中时才广播
        std::cout << "[StateManager] 广播删除: " << rel_path_str << std::endl;
        m_p2p_manager->broadcast_file_delete(rel_path_str);
      }
    }
  }
}

// --- 新增：实现 remove_path_from_map (由 P2PManager 调用) ---
void StateManager::remove_path_from_map(const std::string& relative_path) {
  std::lock_guard<std::mutex> lock(m_file_map_mutex);
  m_file_map.erase(relative_path);
}

void StateManager::scan_directory() {
  std::cout << "[StateManager] Scanning directory: " << m_root_path
            << std::endl;
  std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
  m_file_map.clear();

  // --- 关键修改 ---
  // 为迭代器构造函数提供一个error_code对象，以避免在路径出错时抛出异常
  std::error_code ec;
  auto iterator =
      std::filesystem::recursive_directory_iterator(m_root_path, ec);

  if (ec) {
    std::cerr << "[StateManager] Error creating directory iterator for path "
              << m_root_path << ": " << ec.message() << std::endl;
    return;  // 如果创建迭代器失败，则直接返回
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
    info.path = std::string(reinterpret_cast<const char*>(u8_path_str.c_str()),
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
  std::cout << "[StateManager] Scan complete. Found " << m_file_map.size()
            << " files." << std::endl;
}

std::string StateManager::get_state_as_json_string() {
  // 1. 将map中的FileInfo对象转换成一个vector
  std::vector<FileInfo> files;
  {
    std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
    for (const auto& pair : m_file_map) {
      files.push_back(pair.second);
    }
  }

  // 2. 使用nlohmann/json库构建JSON对象
  nlohmann::json payload;
  payload["files"] = files;  // 这里利用了我们之前定义的 to_json 函数

  nlohmann::json message;
  message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
  message[Protocol::MSG_PAYLOAD] = payload;

  // 3. 将JSON对象序列化为字符串
  return message.dump(2);  // dump(2) 表示使用2个空格进行格式化，便于阅读
}

void StateManager::print_current_state() const {
  std::lock_guard<std::mutex> lock(m_file_map_mutex);
  std::cout << "\n--- Current Directory State ---" << std::endl;
  for (const auto& pair : m_file_map) {
    std::cout << "  - Path: " << pair.second.path << std::endl;
    std::cout << "    MTime: " << pair.second.modified_time << std::endl;
    std::cout << "    Hash: " << pair.second.hash.substr(0, 12) << "..."
              << std::endl;
  }
  std::cout << "-----------------------------" << std::endl;
}
}  // namespace VeritasSync
