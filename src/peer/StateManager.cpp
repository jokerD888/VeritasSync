#include "VeritasSync/StateManager.h"

#include <boost/asio.hpp>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>  // 用于 RENAME 配对

#include "VeritasSync/Hashing.h"
#include "VeritasSync/P2PManager.h"

// 包含 Windows API 和 Asio 适配器
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <boost/asio/windows/object_handle.hpp>

namespace VeritasSync {

// ========================================================================
// 1. 新的事件状态机
// ========================================================================
struct PendingFileState {
  std::chrono::steady_clock::time_point lastEventAt;
  std::chrono::steady_clock::time_point lastWriteAt;
  bool deletedBeforeCommit = false;
  std::unique_ptr<boost::asio::steady_timer> timer;
  std::filesystem::path currentPath;

  PendingFileState(boost::asio::io_context& io_context,
                   const std::filesystem::path& path)
      : timer(std::make_unique<boost::asio::steady_timer>(io_context)),
        currentPath(path) {}
};

class FileEventStateMachine {
 public:
  FileEventStateMachine(P2PManager& p2p_manager)
      : m_p2p_manager(p2p_manager),
        m_rename_grace_ms(2000),       // 2 秒重命名宽限期
        m_write_stable_ms(700),        // 700ms 写入稳定期
        m_rename_link_timeout_ms(500)  // RENAME_OLD 等待 RENAME_NEW 的超时时间
  {
    std::cout << "[StateMachine] 已启动。" << std::endl;
  }

  ~FileEventStateMachine() = default;

  // 核心：处理来自 Watcher 的原始事件
  void on_fs_event(const std::filesystem::path& path, DWORD action) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();

    // --- 重命名链接逻辑 ---
    if (action == FILE_ACTION_RENAMED_NEW_NAME) {
      // 【修复】 使用 .string() 进行日志记录
      std::cout << "[StateMachine] RENAME_NEW: " << path.string() << std::endl;

      // 检查是否有匹配的 RENAME_OLD
      if (m_pending_rename_old.has_value() &&
          (now - m_pending_rename_old->second) < m_rename_link_timeout_ms) {
        std::filesystem::path old_path = m_pending_rename_old->first;
        // 【修复】 使用 .string() 进行日志记录
        std::cout << "[StateMachine] -> 成功链接到 RENAME_OLD: "
                  << old_path.string() << std::endl;

        // 1. 取消并移除 OLD 状态 (如果存在)
        auto old_it = m_pending_states.find(old_path);
        if (old_it != m_pending_states.end()) {
          old_it->second->timer->cancel();
          m_pending_states.erase(old_it);
        }

        // 2. 获取或创建 NEW 状态
        auto& p_new = get_or_create_state(path);
        p_new.lastEventAt = now;
        p_new.deletedBeforeCommit = false;  // 确保它不是删除状态
        p_new.timer->cancel();              // 重置计时器

        // 3. 启动 NEW 状态的稳定计时器
        p_new.timer->expires_after(m_rename_grace_ms);
        p_new.timer->async_wait(
            [this, path](const boost::system::error_code& ec) {
              if (!ec) commit_stable_file(path);
            });

        // 4. 清除 RENAME_OLD 标记
        m_pending_rename_old.reset();
        return;  // 重命名事件处理完毕
      } else {
        std::cout
            << "[StateMachine] -> 未找到匹配的 RENAME_OLD 或已超时，视为 ADD"
            << std::endl;
        // 视为独立的 ADD 事件，继续往下走
        action = FILE_ACTION_ADDED;
        m_pending_rename_old.reset();  // 清除旧标记
      }
    }

    if (action == FILE_ACTION_RENAMED_OLD_NAME) {
      // 【修复】 使用 .string() 进行日志记录
      std::cout << "[StateMachine] RENAME_OLD: " << path.string() << std::endl;

      // 1. 记录 RENAME_OLD 事件，等待 RENAME_NEW
      m_pending_rename_old = {path, now};

      // 2. 获取或创建 OLD 状态，标记为删除，并取消其计时器
      auto& p_old = get_or_create_state(path);
      p_old.deletedBeforeCommit = true;
      p_old.timer->cancel();

      // 【重要】启动一个短计时器，如果 RENAME_NEW 没来，就提交删除
      p_old.timer->expires_after(m_rename_link_timeout_ms);
      p_old.timer->async_wait([this,
                               path](const boost::system::error_code& ec) {
        if (!ec) {
          // RENAME_NEW 没有在超时时间内到达
          std::lock_guard<std::mutex> lock(m_mutex);
          if (m_pending_rename_old.has_value() &&
              m_pending_rename_old->first == path) {
            // 【修复】 使用 .string() 进行日志记录
            std::cout << "[StateMachine] -> RENAME_OLD 超时，提交为 REMOVE: "
                      << path.string() << std::endl;
            commit_stable_file(path);  // 提交删除
            m_pending_rename_old.reset();
          }
        }
      });

      return;  // 等待 RENAME_NEW 或超时
    }

    // --- 常规事件处理 (ADD, MODIFY, REMOVE) ---
    auto& p = get_or_create_state(path);
    p.lastEventAt = now;
    p.timer->cancel();  // 重置现有计时器

    switch (action) {
      case FILE_ACTION_ADDED:
        // 【修复】 使用 .string() 进行日志记录
        std::cout << "[StateMachine] ADD: " << path.string() << std::endl;
        p.deletedBeforeCommit = false;  // 确保不是删除状态
        break;
      case FILE_ACTION_REMOVED:
        // 【修复】 使用 .string() 进行日志记录
        std::cout << "[StateMachine] REMOVE: " << path.string() << std::endl;
        p.deletedBeforeCommit = true;
        break;
      case FILE_ACTION_MODIFIED:
        // 【修复】 使用 .string() 进行日志记录
        std::cout << "[StateMachine] MODIFY: " << path.string() << std::endl;
        p.lastWriteAt = now;
        p.deletedBeforeCommit = false;  // 修改意味着文件存在
        break;
    }

    // 重新启动稳定计时器
    p.timer->expires_after(std::max(m_rename_grace_ms, m_write_stable_ms));
    p.timer->async_wait([this, path](const boost::system::error_code& ec) {
      if (!ec) {
        commit_stable_file(path);
      }
    });
  }

 private:
  PendingFileState& get_or_create_state(const std::filesystem::path& path) {
    auto it = m_pending_states.find(path);
    if (it == m_pending_states.end()) {
      it = m_pending_states
               .emplace(path, std::make_unique<PendingFileState>(
                                  m_p2p_manager.get_io_context(), path))
               .first;
    }
    return *it->second;
  }

  void commit_stable_file(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 再次检查 RENAME_OLD 超时
    if (m_pending_rename_old.has_value() &&
        m_pending_rename_old->first == path) {
      m_pending_rename_old.reset();
    }

    auto it = m_pending_states.find(path);
    if (it == m_pending_states.end()) return;  // 可能已被 RENAME 处理

    PendingFileState& p = *it->second;

    if (p.deletedBeforeCommit) {
      // 【修复】 使用 .string() 进行日志记录
      std::cout << "[StateMachine] 提交: " << path.string() << " (已删除)"
                << std::endl;
    } else {
      // 【修复】 使用 .string() 进行日志记录
      std::cout << "[StateMachine] 提交: " << path.string() << " (稳定)"
                << std::endl;
    }

    post_broadcast();
    m_pending_states.erase(it);
  }

  void post_broadcast() {
    boost::asio::post(m_p2p_manager.get_io_context(),
                      [this]() { m_p2p_manager.broadcast_current_state(); });
  }

  P2PManager& m_p2p_manager;
  std::mutex m_mutex;
  std::map<std::filesystem::path, std::unique_ptr<PendingFileState>>
      m_pending_states;

  std::optional<
      std::pair<std::filesystem::path, std::chrono::steady_clock::time_point>>
      m_pending_rename_old;

  std::chrono::milliseconds m_rename_grace_ms;
  std::chrono::milliseconds m_write_stable_ms;
  std::chrono::milliseconds m_rename_link_timeout_ms;
};

// ========================================================================
// 2. 新的原生 Windows 目录监视器
// ========================================================================
class DirectoryWatcher {
 public:
  DirectoryWatcher(boost::asio::io_context& io_context,
                   const std::filesystem::path& root_path,
                   FileEventStateMachine& state_machine)
      : m_io_context(io_context),
        m_root_path(root_path),
        m_state_machine(state_machine),
        m_dir_handle(INVALID_HANDLE_VALUE),
        m_asio_handle(io_context) {
    m_dir_handle = CreateFileW(
        m_root_path.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);

    if (m_dir_handle == INVALID_HANDLE_VALUE) {
      std::cerr << "[Watcher] 严重错误: 无法打开目录句柄: " << GetLastError()
                << std::endl;
      return;
    }

    m_asio_handle.assign(m_dir_handle);
    start_receive();
  }

  ~DirectoryWatcher() {
    if (m_dir_handle != INVALID_HANDLE_VALUE) {
      m_asio_handle.cancel();
      CloseHandle(m_dir_handle);
    }
  }

 private:
  void start_receive() {
    m_buffer.fill(0);
    ZeroMemory(&m_overlapped, sizeof(m_overlapped));
    BOOL success = ReadDirectoryChangesW(
        m_dir_handle, m_buffer.data(), (DWORD)m_buffer.size(),
        TRUE,  // 监视子目录
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
        NULL, &m_overlapped, NULL);

    DWORD dwError = GetLastError();
    if (!success && dwError != ERROR_IO_PENDING) {
      std::cerr << "[Watcher] 严重错误: ReadDirectoryChangesW 失败: " << dwError
                << std::endl;
      return;
    }

    if (success || dwError == ERROR_IO_PENDING) {
      m_asio_handle.async_wait([this](const boost::system::error_code& ec) {
        if (ec) {
          if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "[Watcher] Asio 错误: " << ec.message() << std::endl;
          }
          return;
        }
        handle_receive();
      });
    }
  }

  void handle_receive() {
    DWORD bytes_transferred = 0;
    BOOL success = GetOverlappedResult(m_dir_handle, &m_overlapped,
                                       &bytes_transferred, FALSE);

    if (!success) {
      DWORD error = GetLastError();
      if (error != ERROR_IO_INCOMPLETE && error != ERROR_OPERATION_ABORTED) {
        std::cerr << "[Watcher] GetOverlappedResult 错误: " << error
                  << std::endl;
      }
      start_receive();
      return;
    }

    if (bytes_transferred == 0) {
      std::cerr << "[Watcher] 警告: ReadDirectoryChangesW 返回 0 "
                   "字节。可能发生缓冲区溢出。"
                << std::endl;
      m_state_machine.on_fs_event(m_root_path,
                                  FILE_ACTION_MODIFIED);  // 粗略通知
    } else {
      FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)m_buffer.data();
      while (info) {
        std::wstring wpath_relative(info->FileName,
                                    info->FileNameLength / sizeof(WCHAR));
        std::filesystem::path relative_path = wpath_relative;

        m_state_machine.on_fs_event(relative_path, info->Action);

        if (info->NextEntryOffset == 0) break;
        info = (FILE_NOTIFY_INFORMATION*)((char*)info + info->NextEntryOffset);
      }
    }
    start_receive();
  }

  boost::asio::io_context& m_io_context;
  std::filesystem::path m_root_path;
  FileEventStateMachine& m_state_machine;

  HANDLE m_dir_handle;
  boost::asio::windows::object_handle m_asio_handle;
  std::array<BYTE, 65536> m_buffer;  // 64KB 缓冲区
  OVERLAPPED m_overlapped = {0};
};

// ========================================================================
// 3. StateManager 实现
// ========================================================================
StateManager::StateManager(const std::string& root_path_utf8,
                           P2PManager& p2p_manager)
    : m_root_path(std::filesystem::u8path(root_path_utf8)) {
  std::error_code ec;
  if (!std::filesystem::exists(m_root_path, ec)) {
    // 【修复】 使用 .string() 进行日志记录
    std::cout << "[StateManager] 根目录 " << m_root_path.string()
              << " 不存在，正在创建。" << std::endl;
    if (!std::filesystem::create_directories(m_root_path, ec) || ec) {
      std::cerr << "[StateManager] 严重错误: 无法创建根目录: " << ec.message()
                << std::endl;
    }
  } else if (ec) {
    std::cerr << "[StateManager] 严重错误: 检查根目录时出错: " << ec.message()
              << std::endl;
  }

  // 1. 创建状态机
  m_state_machine = std::make_unique<FileEventStateMachine>(p2p_manager);
  // 2. 创建监视器，并将其连接到状态机
  m_watcher = std::make_unique<DirectoryWatcher>(p2p_manager.get_io_context(),
                                                 m_root_path, *m_state_machine);
  std::cout << "[StateManager] 已启动 Windows 原生监视器。" << std::endl;
}

StateManager::~StateManager() {
  std::cout << "[StateManager] 正在停止文件监控..." << std::endl;
}

void StateManager::scan_directory() {
  // 【修复】 使用 .string() 进行日志记录
  std::cout << "[StateManager] Scanning directory: " << m_root_path.string()
            << std::endl;
  m_file_map.clear();

  std::error_code ec;
  auto iterator = std::filesystem::recursive_directory_iterator(
      m_root_path, std::filesystem::directory_options::skip_permission_denied,
      ec);

  if (ec) {
    // 【修复】 使用 .string() 进行日志记录
    std::cerr << "[StateManager] Error creating directory iterator for path "
              << m_root_path.string() << ": " << ec.message() << std::endl;
    return;
  }

  const auto end = std::filesystem::recursive_directory_iterator();
  while (iterator != end) {
    std::error_code entry_ec;
    const auto& entry = *iterator;

    bool is_dir = entry.is_directory(entry_ec);
    if (entry_ec) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[StateManager] Error checking entry type: "
                << entry.path().string() << ": " << entry_ec.message()
                << std::endl;
      try {
        iterator.increment(ec);
      } catch (...) {
        break;
      }
      continue;
    }
    bool is_file = entry.is_regular_file(entry_ec);
    if (entry_ec) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[StateManager] Error checking entry type: "
                << entry.path().string() << ": " << entry_ec.message()
                << std::endl;
      try {
        iterator.increment(ec);
      } catch (...) {
        break;
      }
      continue;
    }

    if (!is_dir && !is_file) {
      try {
        iterator.increment(ec);
      } catch (...) {
        break;
      }
      continue;
    }

    FileInfo info;
    std::filesystem::path relative_path =
        std::filesystem::relative(entry.path(), m_root_path);
    // 【修复 C2679 (赋值)】
    // info.path 是 std::string, .u8string() 是 std::u8string
    // 我们必须进行显式转换
    std::u8string u8_path_str = relative_path.u8string();
    info.path = std::string(reinterpret_cast<const char*>(u8_path_str.c_str()),
                            u8_path_str.length());

    auto ftime = entry.last_write_time(entry_ec);
    if (entry_ec) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[StateManager] Error getting mtime: "
                << entry.path().string() << ": " << entry_ec.message()
                << std::endl;
      try {
        iterator.increment(ec);
      } catch (...) {
        break;
      }
      continue;
    }
    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
    info.modified_time = sctp.time_since_epoch().count();

    info.hash = Hashing::CalculateSHA256(entry.path());
    if (info.hash.empty() && !is_dir) {
      // 【修复】 使用 .string() 进行日志记录
      std::cerr << "[StateManager] Error calculating hash: "
                << entry.path().string() << std::endl;
      try {
        iterator.increment(ec);
      } catch (...) {
        break;
      }
      continue;
    }

    m_file_map[info.path] = info;

    try {
      iterator.increment(ec);
      if (ec) {
        std::cerr << "[StateManager] Error incrementing iterator: "
                  << ec.message() << std::endl;
      }
    } catch (const std::filesystem::filesystem_error& e) {
      std::cerr << "[StateManager] Filesystem error during iteration: "
                << e.what() << std::endl;
      break;
    } catch (...) {
      std::cerr << "[StateManager] Unknown error during iteration."
                << std::endl;
      break;
    }
  }
  std::cout << "[StateManager] Scan complete. Found " << m_file_map.size()
            << " items (files/dirs)." << std::endl;
}

std::string StateManager::get_state_as_json_string() {
  std::vector<FileInfo> files;
  for (const auto& pair : m_file_map) {
    files.push_back(pair.second);
  }
  nlohmann::json payload;
  payload["files"] = files;
  nlohmann::json message;
  message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
  message[Protocol::MSG_PAYLOAD] = payload;
  return message.dump(-1);
}

void StateManager::print_current_state() const {
  std::cout << "\n--- Current Directory State ---" << std::endl;
  for (const auto& pair : m_file_map) {
    std::cout << "  - Path: " << pair.second.path << std::endl;
    std::cout << "    MTime: " << pair.second.modified_time << std::endl;
    std::cout << "    Hash: "
              << (pair.second.hash == "DIRECTORY"
                      ? "DIRECTORY"
                      : pair.second.hash.substr(0, 12) + "...")
              << std::endl;
  }
  std::cout << "-----------------------------" << std::endl;
}
}  // namespace VeritasSync