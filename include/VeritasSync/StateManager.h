#pragma once

#include "VeritasSync/Protocol.h" // 我们需要用到FileInfo结构体
#include <filesystem>
#include <string>
#include <map>
#include <functional>
#include <memory> 


namespace efsw {
class FileWatcher;
class FileWatchListener;
}



namespace VeritasSync {
class P2PManager;

  class StateManager {
  public:
    // 构造函数接收一个 P2PManager 的引用
    StateManager(const std::string& root_path, P2PManager& p2p_manager);
    ~StateManager();

    // 扫描同步目录，生成当前所有文件的状态快照
    void scan_directory();

    // 将当前的文件状态打包成一个 share_state 类型的JSON字符串
    std::string get_state_as_json_string();

    // (用于调试) 打印当前所有文件的状态到控制台
    void print_current_state() const;

    const std::filesystem::path& get_root_path() const { return m_root_path; }

  private:
    // 同步目录的根路径
    std::filesystem::path m_root_path;

    // 文件状态的核心存储结构
    // key: 文件的相对路径 (例如 "docs/report.txt")
    // value: 文件的详细信息 (FileInfo)
    std::map<std::string, FileInfo> m_file_map;
    std::unique_ptr<efsw::FileWatcher> m_file_watcher;
    std::unique_ptr<efsw::FileWatchListener> m_listener;
  };

} // namespace VeritasSync