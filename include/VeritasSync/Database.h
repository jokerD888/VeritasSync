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

class Database {
public:
    Database(const std::filesystem::path& db_path);
    ~Database();

    // 获取文件的元数据
    // 如果不存在，返回 std::nullopt
    std::optional<FileMetadata> get_file(const std::string& rel_path);

    // 更新或插入文件元数据 (Upsert)
    void update_file(const std::string& rel_path, const std::string& hash, int64_t mtime);

    // 删除文件元数据
    void remove_file(const std::string& rel_path);

    // 开启/提交事务 (用于批量操作加速)
    void begin_transaction();
    void commit_transaction();

private:
    void init_schema();
    void prepare_statements();
    void finalize_statements();

    sqlite3* m_db = nullptr;
    std::string m_db_path;

    // 预编译语句缓存，极大提升性能
    sqlite3_stmt* m_stmt_get = nullptr;
    sqlite3_stmt* m_stmt_update = nullptr;
    sqlite3_stmt* m_stmt_delete = nullptr;

    std::mutex m_mutex;  // 数据库连接通常不是线程安全的，加锁保护
};

}  // namespace VeritasSync