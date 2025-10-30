#include "VeritasSync/StateManager.h"

#include <boost/asio.hpp>
#include <efsw/efsw.hpp>
#include <iostream>

#include "VeritasSync/Hashing.h"
#include "VeritasSync/P2PManager.h"

namespace VeritasSync {

// UpdateListener 现在更简单了
// 它只负责通知 StateManager 发生了变化，并处理防抖
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

    // 1. 使用 std::u8string_view 构造函数，从 efsw 的 UTF-8 字符串正确构造路径
    std::filesystem::path file_path =
        std::filesystem::path(std::u8string_view(
            reinterpret_cast<const char8_t*>(dir.c_str()), dir.length())) /
        std::u8string_view(reinterpret_cast<const char8_t*>(filename.c_str()),
                           filename.length());

    // 2. 将此正确的路径对象转换为可移植的 UTF-8 字符串 (使用
    // .generic_u8string())
    std::u8string u8_generic_path = file_path.generic_u8string();

    // 3. 将 u8string 转换回 std::string (字节保持不变) 以便存储在
    // m_pending_changes 中
    std::string path_to_store(
        reinterpret_cast<const char*>(u8_generic_path.c_str()),
        u8_generic_path.length());

    // 1. 通知 StateManager 将变化暂存
    //    使用 generic_string() 来获取一个可移植的、使用 / 分隔符的 UTF-8 路径
    m_owner->notify_change_detected(path_to_store);
    // ---------------------------------------------------

    // 2. 重置防抖计时器
    m_debounce_timer.cancel();
    m_debounce_timer.expires_after(std::chrono::milliseconds(1500));

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

// --- StateManager 实现 ---

StateManager::StateManager(const std::string& root_path,
                           P2PManager& p2p_manager, bool enable_watcher)
    : m_root_path(std::filesystem::absolute(root_path)),
      m_p2p_manager(&p2p_manager)  // --- 新增：保存 P2PManager 指针 ---
{
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
}

// --- 新增：实现 get_io_context ---
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
    // full_path_str 现在是包含 UTF-8 字节的 std::string
    // 必须使用 u8string_view 构造函数来创建有效的 path 对象
    std::filesystem::path full_path(std::u8string_view(
        reinterpret_cast<const char8_t*>(full_path_str.c_str()),
        full_path_str.length()));

    std::filesystem::path relative_path;

    if (full_path == m_root_path) continue;

    std::error_code ec;
    relative_path = std::filesystem::relative(full_path, m_root_path, ec);
    if (ec) continue;

    const std::u8string u8_path_str = relative_path.u8string();
    std::string rel_path_str(reinterpret_cast<const char*>(u8_path_str.c_str()),
                             u8_path_str.length());

    // --- 判断是“更新”还是“删除” ---
    if (std::filesystem::exists(full_path, ec) && !ec) {
      if (!std::filesystem::is_regular_file(full_path, ec) || ec) {
        continue;
      }

      FileInfo info;
      info.path = rel_path_str;

      auto ftime = std::filesystem::last_write_time(full_path, ec);
      if (ec) continue;
      auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
      info.modified_time = sctp.time_since_epoch().count();

      info.hash = Hashing::CalculateSHA256(full_path);
      if (info.hash.empty()) {
        // 哈希为空 (可能是被锁定了)，跳过
        std::cerr << "[StateManager] 哈希计算失败 (文件可能被锁定): "
                  << rel_path_str << std::endl;
        continue;
      }

      // 更新 m_file_map 并广播
      m_file_map[info.path] = info;
      std::cout << "[StateManager] 广播更新: " << info.path << std::endl;
      m_p2p_manager->broadcast_file_update(info);

    } else {
      // 文件不存在 (Delete)
      if (m_file_map.erase(rel_path_str) > 0) {
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

// --- 修改：为 scan_directory 添加锁 ---
void StateManager::scan_directory() {
  std::cout << "[StateManager] Scanning directory: " << m_root_path
            << std::endl;

  std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
  m_file_map.clear();

  std::error_code ec;
  auto iterator =
      std::filesystem::recursive_directory_iterator(m_root_path, ec);

  if (ec) {
    std::cerr << "[StateManager] Error creating directory iterator for path "
              << m_root_path << ": " << ec.message() << std::endl;
    return;
  }

  for (const auto& entry : iterator) {
    if (!entry.is_regular_file(ec) || ec) {
      continue;
    }

    FileInfo info;

    std::filesystem::path relative_path =
        std::filesystem::relative(entry.path(), m_root_path);

    const std::u8string u8_path_str = relative_path.u8string();
    info.path = std::string(reinterpret_cast<const char*>(u8_path_str.c_str()),
                            u8_path_str.length());

    auto ftime = std::filesystem::last_write_time(entry, ec);
    if (ec) continue;
    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
    info.modified_time = sctp.time_since_epoch().count();

    info.hash = Hashing::CalculateSHA256(entry.path());
    if (info.hash.empty()) continue;

    m_file_map[info.path] = info;
  }
  std::cout << "[StateManager] Scan complete. Found " << m_file_map.size()
            << " files." << std::endl;
}

// --- 修改：为 get_state_as_json_string 添加锁 ---
std::string StateManager::get_state_as_json_string() {
  std::vector<FileInfo> files;
  {
    std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
    for (const auto& pair : m_file_map) {
      files.push_back(pair.second);
    }
  }
  nlohmann::json payload;
  payload["files"] = files;
  nlohmann::json message;
  message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
  message[Protocol::MSG_PAYLOAD] = payload;
  return message.dump(2);
}

// --- 修改：为 print_current_state 添加锁 ---
void StateManager::print_current_state() const {
  std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
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
