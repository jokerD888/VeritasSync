#pragma once

#include <boost/asio/io_context.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>

#include "VeritasSync/Database.h"
#include "VeritasSync/FileFilter.h"
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
        StateManager(const std::string& root_path, P2PManager& p2p_manager, bool enable_watcher,
                     const std::string& sync_key = "unknown");
        ~StateManager();

        // 扫描同步目录，生成当前所有文件的状态快照
        void scan_directory();

        // 将当前的文件状态打包成一个 share_state 类型的JSON字符串
        std::string get_state_as_json_string();

        // (用于调试) 打印当前所有文件的状态到控制台
        void print_current_state() const;

        const std::filesystem::path& get_root_path() const { return m_root_path; }
        std::set<std::string> get_local_directories() const;

        // --- P2PManager 需要的辅助函数 ---
        boost::asio::io_context& get_io_context();
        void add_dir_to_map(const std::string& relative_path);
        void remove_dir_from_map(const std::string& relative_path);

        // (供 P2PManager::handle_file_delete 调用)
        void remove_path_from_map(const std::string& relative_path);

        // 供 P2PManager 检查是否为回声
        bool should_ignore_echo(const std::string& peer_id, const std::string& path, const std::string& remote_hash);

        // 供 TransferManager 记录发送历史
        void record_file_sent(const std::string& peer_id, const std::string& path, const std::string& hash);

        // 供 TransferManager 获取当前文件的 Hash (用于记录)
        std::string get_file_hash(const std::string& relative_path) const;

        // 获取上次同步的哈希 (Base Hash)
        // 本质上就是复用上一阶段的 get_last_sent_hash，为了保持兼容，我们可以直接增加这个 wrapper
        std::string get_base_hash(const std::string& peer_id, const std::string& path);

        // 记录同步成功 (更新 Base Hash)
        void record_sync_success(const std::string& peer_id, const std::string& path, const std::string& hash);

        void clear_sync_history(const std::string& path);

        std::optional<SyncHistory> get_full_history(const std::string& peer_id, const std::string& path);

        // 获取所有文件列表（用于分批推送）
        std::vector<FileInfo> get_all_files() const;

    private:
        // --- 供 UpdateListener 调用的内部方法 ---
        friend class UpdateListener;
        void notify_change_detected(const std::string& full_path);
        void process_debounced_changes();

        // --- 成员变量 ---
        std::string m_sync_key;
        std::filesystem::path m_root_path;
        P2PManager* m_p2p_manager;  // 保存 P2PManager 的指针

        // 文件状态的核心存储结构
        std::map<std::string, FileInfo> m_file_map;
        std::unique_ptr<Database> m_db;
        mutable std::mutex m_file_map_mutex;  // 保护 m_file_map

        std::set<std::string> m_dir_map;
        mutable std::mutex m_dir_map_mutex;

        // 文件监控器
        std::unique_ptr<efsw::FileWatcher> m_file_watcher;
        std::unique_ptr<efsw::FileWatchListener> m_listener;

        // 待处理变更的集合 (优化：使用 unordered_set 提升查找性能)
        std::unordered_set<std::string> m_pending_changes;
        std::mutex m_changes_mutex;

        // 过滤器实例
        FileFilter m_file_filter;
    };

}  // namespace VeritasSync
