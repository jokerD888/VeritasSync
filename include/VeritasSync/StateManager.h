#pragma once

#include <boost/asio/io_context.hpp>  // 需要包含
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "VeritasSync/Protocol.h"

namespace efsw {
class FileWatcher;
class FileWatchListener;
}  // namespace efsw

namespace VeritasSync {
class P2PManager;

class StateManager {
 public:
  // 构造函数接收一个 P2PManager 的引用
  StateManager(const std::string& root_path, P2PManager& p2p_manager,
               bool enable_watcher);
  ~StateManager();

  // 扫描同步目录，生成当前所有文件的状态快照
  void scan_directory();

  // 将当前的文件状态打包成一个 share_state 类型的JSON字符串
  std::string get_state_as_json_string();

  // (用于调试) 打印当前所有文件的状态到控制台
  void print_current_state() const;

  const std::filesystem::path& get_root_path() const { return m_root_path; }

  // --- 新增：P2PManager 需要的辅助函数 ---
  boost::asio::io_context& get_io_context();
  // (供 P2PManager::handle_file_delete 调用)
  void remove_path_from_map(const std::string& relative_path);

 private:
  // --- 新增：供 UpdateListener 调用的内部方法 ---
  friend class UpdateListener;
  void notify_change_detected(const std::string& full_path);
  void process_debounced_changes();

  // --- 成员变量 ---
  std::filesystem::path m_root_path;
  P2PManager* m_p2p_manager;  // 保存 P2PManager 的指针

  // 文件状态的核心存储结构
  std::map<std::string, FileInfo> m_file_map;
  mutable std::mutex m_file_map_mutex;  // 保护 m_file_map

  // 文件监控器
  std::unique_ptr<efsw::FileWatcher> m_file_watcher;
  std::unique_ptr<efsw::FileWatchListener> m_listener;

  // 待处理变更的集合
  std::set<std::string> m_pending_changes;
  std::mutex m_changes_mutex;
};

}  // namespace VeritasSync
