#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/steady_timer.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <atomic>

#include "VeritasSync/storage/Database.h"
#include "VeritasSync/storage/FileFilter.h"
#include "VeritasSync/sync/Protocol.h"

namespace efsw {
    class FileWatcher;
    class FileWatchListener;
}  // namespace efsw

namespace VeritasSync {
    class StateManagerEnhancedTest;

    // ═══════════════════════════════════════════════════════════════
    // 回调结构体：解耦 StateManager 与 P2PManager
    // StateManager 通过这些回调通知外部"发生了什么变更"，
    // 而不需要知道 P2PManager 的存在。
    // ═══════════════════════════════════════════════════════════════
    struct StateManagerCallbacks {
        std::function<void(const std::vector<FileInfo>&)> on_file_updates;
        std::function<void(const std::vector<std::string>&)> on_file_deletes;
        std::function<void(const std::vector<std::string>&, const std::vector<std::string>&)> on_dir_changes;
    };

    class StateManager {
        friend class StateManagerEnhancedTest;
    public:
        // 构造函数接收 io_context 引用和回调，不再依赖 P2PManager
        StateManager(const std::string& root_path, boost::asio::io_context& io_context,
                     StateManagerCallbacks callbacks, bool enable_watcher,
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

        // --- 回声抑制（源头抑制） ---
        // 下载完成后调用，记录接收到的文件（用于抑制后续的回声广播）
        void mark_file_received(const std::string& path, const std::string& hash);
        
        // 检查是否为接收文件的回声，如果是则删除记录并返回 true
        bool check_and_clear_echo(const std::string& path, const std::string& hash);

    private:
        // --- 供 UpdateListener 调用的内部方法 ---
        friend class UpdateListener;
        void notify_change_detected(const std::string& full_path);
        void process_debounced_changes();

        // 统一调度重试任务 (线程安全，内部使用 post)
        void schedule_retry(int delay_seconds);

        // --- 成员变量 ---
        std::string m_sync_key;
        std::filesystem::path m_root_path;
        boost::asio::io_context& m_io_context;  // 外部注入的 io_context
        StateManagerCallbacks m_callbacks;       // 变更通知回调

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

        // --- 回声抑制（源头抑制） ---
        // 记录从对端接收到的文件（path → hash），用于在广播前检查并跳过回声
        std::unordered_map<std::string, std::string> m_received_files;
        std::mutex m_received_files_mutex;

        // --- 生命周期管理 ---
        // 唯一的重试计时器，负责调度所有文件冲突后的再次处理任务
        std::unique_ptr<boost::asio::steady_timer> m_retry_timer;
        
        // --- 阈值触发机制 ---
        // 上次批处理时间点（用于限制触发频率）
        std::chrono::steady_clock::time_point m_last_batch_time;
        // 是否正在处理中（防止重入）
        std::atomic<bool> m_processing{false};
        
        // 检查是否需要立即触发批处理（在 notify_change_detected 调用后检查）
        // 返回 true 表示需要触发
        bool check_and_trigger_batch();
    };

}  // namespace VeritasSync
