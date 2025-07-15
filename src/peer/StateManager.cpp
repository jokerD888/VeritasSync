#include "VeritasSync/StateManager.h"
#include "VeritasSync/Hashing.h"
#include <iostream>

namespace VeritasSync {

  StateManager::StateManager(const std::string& root_path)
    : m_root_path(root_path) {
    // 确保同步目录存在，如果不存在则创建它
    if (!std::filesystem::exists(m_root_path)) {
      std::cout << "[StateManager] Root path " << m_root_path << " does not exist. Creating it." << std::endl;
      std::filesystem::create_directory(m_root_path);
    }
  }

  // 在 src/peer/StateManager.cpp 中

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
      info.path = std::filesystem::relative(entry.path(), m_root_path).generic_string();

      // 2. 获取最后修改时间
      auto ftime = std::filesystem::last_write_time(entry, ec);
      if (ec) continue; // 如果获取时间失败，跳过
      auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
      info.modified_time = sctp.time_since_epoch().count();

      // 3. 计算哈希值
      info.hash = Hashing::CalculateSHA256(entry.path());
      if (info.hash.empty()) continue; // 如果计算哈希失败，跳过

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