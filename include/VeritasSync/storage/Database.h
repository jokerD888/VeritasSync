#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

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

class Database {
public:
    Database(const std::filesystem::path& db_path, int busy_timeout_ms = 5000);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 【修复】检查数据库是否成功初始化
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
    class TransactionGuard {
    public:
        TransactionGuard(Database& db);
        ~TransactionGuard();
        void commit();
    private:
        Database& m_db;
        std::unique_lock<std::recursive_mutex> m_lock; // 自动管理锁的生命周期
        bool m_committed = false;
    };

    void begin_transaction();
    void commit_transaction();
    void rollback_transaction();
    bool update_sync_history(const std::string& peer_id, const std::string& path, const std::string& hash);
    bool remove_sync_history(const std::string& path);

private:
    void init_schema();
    void prepare_statements();

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
    
    mutable std::recursive_mutex m_mutex; 
};

}  // namespace VeritasSync