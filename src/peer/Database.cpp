#include "VeritasSync/Database.h"

#include <sqlite3.h>

#include <iostream>

#include "VeritasSync/Logger.h"

namespace VeritasSync {

Database::Database(const std::filesystem::path& db_path) : m_db_path(db_path.string()) {
    // 打开数据库 (如果不存在则创建)
    int rc = sqlite3_open(m_db_path.c_str(), &m_db);
    if (rc) {
        g_logger->error("[Database] 无法打开数据库 {}: {}", m_db_path, sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }

    // 初始化表结构
    init_schema();
    // 预编译 SQL 语句
    prepare_statements();

    g_logger->info("[Database] 数据库已加载: {}", m_db_path);
}

Database::~Database() {
    finalize_statements();
    if (m_db) {
        sqlite3_close(m_db);
    }
}

void Database::init_schema() {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS files ("
        "  path TEXT PRIMARY KEY,"
        "  hash TEXT NOT NULL,"
        "  mtime INTEGER NOT NULL"
        ");"
        // 创建索引可以加速路径查找 (主键已有默认索引，这里作为演示)
        // "CREATE INDEX IF NOT EXISTS idx_path ON files(path);"
        ;

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        g_logger->error("[Database] SQL 错误: {}", errMsg);
        sqlite3_free(errMsg);
    }
}

void Database::prepare_statements() {
    const char* sql_get = "SELECT hash, mtime FROM files WHERE path = ?;";
    sqlite3_prepare_v2(m_db, sql_get, -1, &m_stmt_get, nullptr);

    const char* sql_update = "INSERT OR REPLACE INTO files (path, hash, mtime) VALUES (?, ?, ?);";
    sqlite3_prepare_v2(m_db, sql_update, -1, &m_stmt_update, nullptr);

    const char* sql_delete = "DELETE FROM files WHERE path = ?;";
    sqlite3_prepare_v2(m_db, sql_delete, -1, &m_stmt_delete, nullptr);
}

void Database::finalize_statements() {
    if (m_stmt_get) sqlite3_finalize(m_stmt_get);
    if (m_stmt_update) sqlite3_finalize(m_stmt_update);
    if (m_stmt_delete) sqlite3_finalize(m_stmt_delete);
}

std::optional<FileMetadata> Database::get_file(const std::string& rel_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_stmt_get) return std::nullopt;

    // 绑定参数
    sqlite3_reset(m_stmt_get);
    sqlite3_bind_text(m_stmt_get, 1, rel_path.c_str(), -1, SQLITE_STATIC);

    // 执行查询
    if (sqlite3_step(m_stmt_get) == SQLITE_ROW) {
        FileMetadata meta;
        meta.path = rel_path;
        meta.hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get, 0));
        meta.mtime = sqlite3_column_int64(m_stmt_get, 1);
        return meta;
    }

    return std::nullopt;
}

void Database::update_file(const std::string& rel_path, const std::string& hash, int64_t mtime) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_stmt_update) return;

    sqlite3_reset(m_stmt_update);
    sqlite3_bind_text(m_stmt_update, 1, rel_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_update, 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_update, 3, mtime);

    if (sqlite3_step(m_stmt_update) != SQLITE_DONE) {
        g_logger->error("[Database] Update 失败: {}", sqlite3_errmsg(m_db));
    }
}

void Database::remove_file(const std::string& rel_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_stmt_delete) return;

    sqlite3_reset(m_stmt_delete);
    sqlite3_bind_text(m_stmt_delete, 1, rel_path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_delete) != SQLITE_DONE) {
        g_logger->error("[Database] Delete 失败: {}", sqlite3_errmsg(m_db));
    }
}

void Database::begin_transaction() {
    std::lock_guard<std::mutex> lock(m_mutex);
    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
}

void Database::commit_transaction() {
    std::lock_guard<std::mutex> lock(m_mutex);
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

}  // namespace VeritasSync