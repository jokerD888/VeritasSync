#include "VeritasSync/StateManager.h"

#include <boost/asio.hpp>
#include <efsw/efsw.hpp>
#include <iostream>

#include "VeritasSync/Hashing.h"
#include "VeritasSync/P2PManager.h"
#include "VeritasSync/Logger.h" // <-- 新增

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

        void handleFileAction(
            efsw::WatchID, const std::string& dir, const std::string& filename,
            efsw::Action action,
            std::string oldFilename) override {  // <-- 启用 oldFilename
            if (filename == "." || filename == "..") {
                return;
            }

            if (action == efsw::Actions::Moved && !oldFilename.empty()) {
                // 这是一个重命名，我们必须把 "oldFilename" 也作为一个变更来通知
                // StateManager 稍后会检查它，发现它 "不存在"，并将其作为 "Delete" 处理
                std::filesystem::path old_file_path =
                    std::filesystem::path(std::u8string_view(
                        reinterpret_cast<const char8_t*>(dir.c_str()), dir.length())) /
                    std::u8string_view(
                        reinterpret_cast<const char8_t*>(oldFilename.c_str()),
                        oldFilename.length());

                std::u8string u8_generic_old_path = old_file_path.generic_u8string();
                std::string old_path_to_store(
                    reinterpret_cast<const char*>(u8_generic_old_path.c_str()),
                    u8_generic_old_path.length());

                m_owner->notify_change_detected(old_path_to_store);
                // 注意：我们没有 'return'，因为我们紧接着要处理 'filename' (新文件)
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
            m_debounce_timer.expires_after(std::chrono::milliseconds(5000));

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
            g_logger->info("[StateManager] 根目录 {} 不存在，正在创建。", m_root_path.string());
            std::filesystem::create_directory(m_root_path);
        }

        if (enable_watcher) {
            m_file_watcher = std::make_unique<efsw::FileWatcher>();
            // --- 修改：将 'this' 传递给 UpdateListener ---
            m_listener = std::make_unique<UpdateListener>(this);
            m_file_watcher->addWatch(m_root_path.string(), m_listener.get(), true);
            m_file_watcher->watch();
            g_logger->info("[StateManager] 已启动对目录 '{}' 的实时监控 (Source 模式)。", m_root_path.string());
        } else {
            g_logger->info("[StateManager] 以 'Destination' 模式启动，文件监控已禁用。");
        }
    }

    StateManager::~StateManager() {
        if (m_file_watcher) {
            g_logger->info("[StateManager] 正在停止文件监控...");
        }
    }

    std::set<std::string> StateManager::get_local_directories() const {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        return m_dir_map;
    }

    void StateManager::add_dir_to_map(const std::string& relative_path) {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        m_dir_map.insert(relative_path);
    }

    void StateManager::remove_dir_from_map(const std::string& relative_path) {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        m_dir_map.erase(relative_path);
    }

    // --- 新增：实现 get_io_context ---
    boost::asio::io_context& StateManager::get_io_context() {
        return m_p2p_manager->get_io_context();
    }

    // --- 新增：实现 notify_change_detected (由 UpdateListener 调用) ---
    void StateManager::notify_change_detected(const std::string& full_path) {
        std::lock_guard<std::mutex> lock(m_changes_mutex);
        m_pending_changes.insert(full_path);
        // 使用 Debug 级别，因为这个日志非常频繁
        g_logger->debug("[Watcher] 检测到变化: {}", full_path);
    }

    // --- 新增：实现 process_debounced_changes (由 UpdateListener 定时器调用) ---
    void StateManager::process_debounced_changes() {
        g_logger->info("[Watcher] 文件系统稳定，正在处理增量变化...");

        std::set<std::string> changes_to_process;
        {
            std::lock_guard<std::mutex> lock(m_changes_mutex);
            // 交换数据，快速释放锁
            m_pending_changes.swap(changes_to_process);
        }

        std::lock_guard<std::mutex> file_lock(m_file_map_mutex);  // <-- 重命名
        std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);    // <-- 新增

        for (const auto& full_path_str : changes_to_process) {
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
                if (std::filesystem::is_regular_file(full_path, ec) || ec) {
                    FileInfo info;
                    info.path = rel_path_str;

                    auto ftime = std::filesystem::last_write_time(full_path, ec);
                    if (ec) continue;
                    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
                    info.modified_time = sctp.time_since_epoch().count();                       

                    info.hash = Hashing::CalculateSHA256(full_path);
                    if (info.hash.empty()) {
                        // 哈希为空 (可能是被锁定了)，跳过
                        g_logger->warn("[StateManager] 哈希计算失败 (文件可能被锁定): {}", rel_path_str);
                        continue;
                    }

                    // 更新 m_file_map 并广播
                    m_file_map[info.path] = info;
                    g_logger->info("[StateManager] 广播更新: {}", info.path);
                    m_p2p_manager->broadcast_file_update(info);
                } else if (std::filesystem::is_directory(full_path, ec) && !ec) {
                    // --- 新增：目录创建逻辑 ---
                    if (m_dir_map.find(rel_path_str) == m_dir_map.end()) {
                        m_dir_map.insert(rel_path_str);
                        g_logger->info("[StateManager] 广播目录创建: {}", rel_path_str);
                        m_p2p_manager->broadcast_dir_create(rel_path_str);
                    }
                }

            } else {
                // 文件或目录不存在 (Delete)
                if (m_file_map.erase(rel_path_str) > 0) {
                    g_logger->info("[StateManager] 广播删除: {}", rel_path_str);
                    m_p2p_manager->broadcast_file_delete(rel_path_str);
                } else if (m_dir_map.erase(rel_path_str) > 0) {
                    // --- 新增：目录删除逻辑 ---
                    g_logger->info("[StateManager] 广播目录删除: {}", rel_path_str);
                    m_p2p_manager->broadcast_dir_delete(rel_path_str);
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
        g_logger->info("[StateManager] Scanning directory: {}", m_root_path.string());

        std::lock_guard<std::mutex> file_lock(m_file_map_mutex);  // <-- 重命名
        std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);    // <-- 新增
        m_file_map.clear();
        m_dir_map.clear();  // <-- 新增

        std::error_code ec;
        auto iterator =
            std::filesystem::recursive_directory_iterator(m_root_path, ec);

        if (ec) {
            g_logger->error("[StateManager] Error creating directory iterator for path {}: {}", m_root_path.string(), ec.message());
            return;
        }

        for (const auto& entry : iterator) {
            std::filesystem::path relative_path =
                std::filesystem::relative(entry.path(), m_root_path);
            const std::u8string u8_path_str = relative_path.u8string();
            std::string rel_path_str(reinterpret_cast<const char*>(u8_path_str.c_str()),
                u8_path_str.length());

            if (rel_path_str.empty()) continue;  // 忽略根目录本身

            if (entry.is_regular_file(ec) && !ec) {
                FileInfo info;

                info.path =
                    std::string(reinterpret_cast<const char*>(u8_path_str.c_str()),
                        u8_path_str.length());

                auto ftime = std::filesystem::last_write_time(entry, ec);
                if (ec) continue;
                auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
                info.modified_time = sctp.time_since_epoch().count();

                info.hash = Hashing::CalculateSHA256(entry.path());
                if (info.hash.empty()) continue;

                m_file_map[info.path] = info;
            } else if (entry.is_directory(ec) && !ec) {
                // ---跟踪目录 ---
                m_dir_map.insert(rel_path_str);
            }
        }
        g_logger->info("[StateManager] Scan complete. Found {} files and {} directories.", m_file_map.size(), m_dir_map.size());
    }

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

        {
            std::lock_guard<std::mutex> lock(m_dir_map_mutex);
            payload["directories"] = m_dir_map;
        }

        nlohmann::json message;
        message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
        message[Protocol::MSG_PAYLOAD] = payload;
        return message.dump(2);
    }

    // --- 修改：为 print_current_state 添加锁 ---
    void StateManager::print_current_state() const {
        std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
        g_logger->info("--- Current Directory State ---");
        for (const auto& pair : m_file_map) {
            g_logger->info("  - Path: {}", pair.second.path);
            g_logger->info("    MTime: {}", pair.second.modified_time);
            g_logger->info("    Hash: {}...", pair.second.hash.substr(0, 12));
        }
        g_logger->info("-----------------------------");
    }
}  // namespace VeritasSync
