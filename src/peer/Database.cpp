#include "VeritasSync/Database.h"

#include <sqlite3.h>

#include <iostream>

#include "VeritasSync/EncodingUtils.h"
#include "VeritasSync/Logger.h"

namespace VeritasSync {

Database::Database(const std::filesystem::path& db_path) : m_db_path(db_path.string()) {
    // 注意：成员变量 m_db_path 只是为了存个日志用的 string，转成 UTF-8 存起来
    m_db_path = PathToUtf8(db_path);

    int rc = 0;
#ifdef _WIN32
    // 【关键】Windows 上使用宽字符接口打开数据库
    std::wstring w_path = Utf8ToWide(m_db_path);
    rc = sqlite3_open16(w_path.c_str(), &m_db);
#else
    // Linux 上直接用 UTF-8
    rc = sqlite3_open(m_db_path.c_str(), &m_db);
#endif
    if (rc) {
        g_logger->error("[Database] 无法打开数据库 {}: {}", m_db_path, sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }
    // --- 优化: 启用 WAL 模式和 Normal 同步 ---
    // WAL: 大幅提升并发性能，避免读写阻塞
    // NORMAL: 在保证安全性的前提下减少 fsync 次数
    char* errMsg = nullptr;
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    if (errMsg) {
        g_logger->warn("[Database] 启用 WAL 模式失败: {}", errMsg);
        sqlite3_free(errMsg);
    } else {
        sqlite3_exec(m_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
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
    // 1. files 表
    const char* sql_files =
        "CREATE TABLE IF NOT EXISTS files ("
        "  path TEXT PRIMARY KEY,"
        "  hash TEXT NOT NULL,"
        "  mtime INTEGER NOT NULL"
        ");";

    // 2. sync_history 表
    // 记录: 我们最后一次成功发送给 peer_id 的 path 文件的 hash
    const char* sql_history =
        "CREATE TABLE IF NOT EXISTS sync_history ("
        "  peer_id TEXT,"
        "  path TEXT,"
        "  hash TEXT,"
        "  ts INTEGER,"
        "  PRIMARY KEY(peer_id, path)"
        ");";

    char* errMsg = nullptr;

    // 执行建表
    sqlite3_exec(m_db, sql_files, nullptr, nullptr, &errMsg);
    sqlite3_exec(m_db, sql_history, nullptr, nullptr, &errMsg);

    if (errMsg) {
        g_logger->error("[Database] SQL Error: {}", errMsg);
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

    // 记录发送历史 (Upsert)
    const char* sql_hist_update = "INSERT OR REPLACE INTO sync_history (peer_id, path, hash, ts) VALUES (?, ?, ?, ?);";
    sqlite3_prepare_v2(m_db, sql_hist_update, -1, &m_stmt_hist_update, nullptr);

    // 查询发送历史
    const char* sql_hist_get = "SELECT hash FROM sync_history WHERE peer_id = ? AND path = ?;";
    sqlite3_prepare_v2(m_db, sql_hist_get, -1, &m_stmt_hist_get, nullptr);
}

void Database::finalize_statements() {
    if (m_stmt_get) sqlite3_finalize(m_stmt_get);
    if (m_stmt_update) sqlite3_finalize(m_stmt_update);
    if (m_stmt_delete) sqlite3_finalize(m_stmt_delete);
    if (m_stmt_hist_update) sqlite3_finalize(m_stmt_hist_update);
    if (m_stmt_hist_get) sqlite3_finalize(m_stmt_hist_get);
}

void Database::update_sync_history(const std::string& peer_id, const std::string& path, const std::string& hash) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_update) return;

    sqlite3_reset(m_stmt_hist_update);
    sqlite3_bind_text(m_stmt_hist_update, 1, peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update, 2, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update, 3, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_hist_update, 4, std::time(nullptr));  // 当前时间戳

    sqlite3_step(m_stmt_hist_update);
}
std::string Database::get_last_sent_hash(const std::string& peer_id, const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_get) return "";

    sqlite3_reset(m_stmt_hist_get);
    sqlite3_bind_text(m_stmt_hist_get, 1, peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_get, 2, path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_hist_get) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_hist_get, 0));
        return val ? std::string(val) : "";
    }
    return "";
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