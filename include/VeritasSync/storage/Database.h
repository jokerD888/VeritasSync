#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// 前向声明，避免在头文件中引入 sqlite3.h
struct sqlite3;
struct sqlite3_stmt;

namespace VeritasSync {

// 对应数据库中的一行记录
struct FileMetadata {
    std::string path;
    std::string hash;
    int64_t mtime;
    // 可以扩展 size 等字段
};

struct SyncHistory {
    std::string hash;
    int64_t ts = 0;  // 时间戳
};

/// 断点续传：下载任务的持久化记录
struct DownloadTaskRecord {
    std::string path;
    std::string peer_id;
    uint32_t total_chunks = 0;
    std::vector<uint8_t> bitmap_data;  // 序列化的 received_bitmap
    std::string expected_hash;
    uint64_t expected_size = 0;
    std::string temp_path;
    int64_t last_active = 0;
};

class Database {
public:
    Database(const std::filesystem::path& db_path, int busy_timeout_ms = 5000);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 检查数据库是否成功初始化
    bool is_valid() const { return m_db != nullptr; }

    // 获取文件的元数据 (标记为 const，因为不改变 DB 状态)
    std::optional<FileMetadata> get_file(const std::string& rel_path) const;
    std::optional<SyncHistory> get_sync_history(const std::string& peer_id, const std::string& path) const;
    std::string get_last_sent_hash(const std::string& peer_id, const std::string& path) const;

    // 更新或插入文件元数据 (Upsert)，返回 false 表示写入失败
    bool update_file(const std::string& rel_path, const std::string& hash, int64_t mtime);
    bool remove_file(const std::string& rel_path);
    std::vector<std::string> get_all_file_paths() const;
    // 批量获取所有文件元数据（单次 SQL 查询，避免 N+1）
    std::vector<FileMetadata> get_all_files() const;

    // 开启事务的安全 Guard
    // 注意：事务全程持有 m_mutex，用于保护8 个共享的 sqlite3_stmt* 成员变量
    // 共享 stmts 要求同一时间只能有一个线程操作 Database 对象。
    class TransactionGuard {
    public:
        TransactionGuard(Database& db);
        ~TransactionGuard();
        bool commit();  // 返回 false 表示落盘失败
    private:
        Database& m_db;
        std::unique_lock<std::recursive_mutex> m_lock;
        bool m_committed = false;
    };

    bool update_sync_history(const std::string& peer_id, const std::string& path, const std::string& hash);
    bool remove_sync_history(const std::string& path);

    // --- 断点续传持久化 ---
    bool save_download_task(const DownloadTaskRecord& task);
    bool remove_download_task(const std::string& path);
    std::vector<DownloadTaskRecord> load_all_download_tasks() const;

private:
    void init_schema();
    void prepare_statements();
    // 无锁版本的事务操作，供 TransactionGuard 直接调用
    bool begin_transaction_nolock();
    bool commit_transaction_nolock();
    bool rollback_transaction_nolock();

    sqlite3* m_db = nullptr;
    std::string m_db_path;

    // 使用 RAII 自动管理 SQL 语句资源
    struct StmtDeleter { void operator()(sqlite3_stmt* s); };
    using ScopedStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

    ScopedStmt m_stmt_get;
    ScopedStmt m_stmt_update;
    ScopedStmt m_stmt_delete;
    ScopedStmt m_stmt_hist_update;
    ScopedStmt m_stmt_hist_get;
    ScopedStmt m_stmt_hist_delete;
    ScopedStmt m_stmt_get_all_paths;
    ScopedStmt m_stmt_get_all_files;

    // 断点续传
    ScopedStmt m_stmt_save_task;
    ScopedStmt m_stmt_remove_task;
    ScopedStmt m_stmt_load_all_tasks;
    
    mutable std::recursive_mutex m_mutex; 
};

}  // namespace VeritasSync