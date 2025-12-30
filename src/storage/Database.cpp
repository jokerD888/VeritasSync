#include "VeritasSync/storage/Database.h"

#include <sqlite3.h>

#include <iostream>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {
// --- StmtDeleter 实现 ---
void Database::StmtDeleter::operator()(sqlite3_stmt* s) {
    if (s) {
        sqlite3_finalize(s);
    }
}

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

    // --- 设置繁忙重试超时 (5秒) ---
    // 防止多线程写入时出现 "database is locked" 错误
    sqlite3_busy_timeout(m_db, 5000);

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

    // 3. 索引优化 (关键！)
    // 理由：remove_sync_history 仅通过 path 删除。
    // 原有的主键索引是 (peer_id, path)，无法加速仅按 path 的查询。
    // 增加此索引可将全表扫描优化为索引查找，在大数据量下性能提升百倍。
    const char* sql_index_path = "CREATE INDEX IF NOT EXISTS idx_sync_history_path ON sync_history(path);";

    char* errMsg = nullptr;

    // 执行建表和索引创建
    sqlite3_exec(m_db, sql_files, nullptr, nullptr, &errMsg);
    sqlite3_exec(m_db, sql_history, nullptr, nullptr, &errMsg);
    sqlite3_exec(m_db, sql_index_path, nullptr, nullptr, &errMsg);

    if (errMsg) {
        g_logger->error("[Database] SQL Error: {}", errMsg);
        sqlite3_free(errMsg);
    }
}

void Database::prepare_statements() {
    const char* sql_get = "SELECT hash, mtime FROM files WHERE path = ?;";
    sqlite3_stmt* stmt_get = nullptr;
    sqlite3_prepare_v2(m_db, sql_get, -1, &stmt_get, nullptr);
    m_stmt_get.reset(stmt_get);

    const char* sql_update = "INSERT OR REPLACE INTO files (path, hash, mtime) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt_update = nullptr;
    sqlite3_prepare_v2(m_db, sql_update, -1, &stmt_update, nullptr);
    m_stmt_update.reset(stmt_update);

    const char* sql_delete = "DELETE FROM files WHERE path = ?;";
    sqlite3_stmt* stmt_delete = nullptr;
    sqlite3_prepare_v2(m_db, sql_delete, -1, &stmt_delete, nullptr);
    m_stmt_delete.reset(stmt_delete);

    // 记录发送历史 (Upsert)
    const char* sql_hist_update = "INSERT OR REPLACE INTO sync_history (peer_id, path, hash, ts) VALUES (?, ?, ?, STRFTIME('%s','now'));";
    sqlite3_stmt* stmt_hist_update = nullptr;
    sqlite3_prepare_v2(m_db, sql_hist_update, -1, &stmt_hist_update, nullptr);
    m_stmt_hist_update.reset(stmt_hist_update);

    // 查询发送历史
    const char* sql_hist_get = "SELECT hash, ts FROM sync_history WHERE peer_id = ? AND path = ?;";
    sqlite3_stmt* stmt_hist_get = nullptr;
    sqlite3_prepare_v2(m_db, sql_hist_get, -1, &stmt_hist_get, nullptr);
    m_stmt_hist_get.reset(stmt_hist_get);

    // 删除历史
    const char* sql_hist_delete = "DELETE FROM sync_history WHERE path = ?;";
    sqlite3_stmt* stmt_hist_delete = nullptr;
    sqlite3_prepare_v2(m_db, sql_hist_delete, -1, &stmt_hist_delete, nullptr);
    m_stmt_hist_delete.reset(stmt_hist_delete);

    const char* sql_get_all = "SELECT path FROM files;";
    sqlite3_stmt* stmt_get_all = nullptr;
    sqlite3_prepare_v2(m_db, sql_get_all, -1, &stmt_get_all, nullptr);
    m_stmt_get_all_paths.reset(stmt_get_all);
}
std::optional<SyncHistory> Database::get_sync_history(const std::string& peer_id, const std::string& path) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_get) return std::nullopt;

    sqlite3_reset(m_stmt_hist_get.get());
    sqlite3_bind_text(m_stmt_hist_get.get(), 1, peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_get.get(), 2, path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_hist_get.get()) == SQLITE_ROW) {
        SyncHistory hist;
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_hist_get.get(), 0));
        hist.hash = val ? std::string(val) : "";
        hist.ts = sqlite3_column_int64(m_stmt_hist_get.get(), 1);
        return hist;
    }
    return std::nullopt;
}
void Database::remove_sync_history(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_delete) return;

    sqlite3_reset(m_stmt_hist_delete.get());
    sqlite3_bind_text(m_stmt_hist_delete.get(), 1, path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_hist_delete.get()) != SQLITE_DONE) {
        g_logger->error("[Database] 删除历史记录失败: {}", sqlite3_errmsg(m_db));
    }
}
void Database::update_sync_history(const std::string& peer_id, const std::string& path, const std::string& hash) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_update) return;

    sqlite3_reset(m_stmt_hist_update.get());
    sqlite3_bind_text(m_stmt_hist_update.get(), 1, peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update.get(), 2, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update.get(), 3, hash.c_str(), -1, SQLITE_STATIC);
    // 第 4 个参数 ts 已由 SQL 内置函数 STRFTIME 处理，无需 C++ 绑定

    sqlite3_step(m_stmt_hist_update.get());
}
std::string Database::get_last_sent_hash(const std::string& peer_id, const std::string& path) const {
    auto res = get_sync_history(peer_id, path);
    return res ? res->hash : "";
}
std::optional<FileMetadata> Database::get_file(const std::string& rel_path) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_get) return std::nullopt;

    // 绑定参数
    sqlite3_reset(m_stmt_get.get());
    sqlite3_bind_text(m_stmt_get.get(), 1, rel_path.c_str(), -1, SQLITE_STATIC);

    // 执行查询
    if (sqlite3_step(m_stmt_get.get()) == SQLITE_ROW) {
        FileMetadata meta;
        meta.path = rel_path;
        meta.hash = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get.get(), 0));
        meta.mtime = sqlite3_column_int64(m_stmt_get.get(), 1);
        return meta;
    }

    return std::nullopt;
}

void Database::update_file(const std::string& rel_path, const std::string& hash, int64_t mtime) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_update) return;

    sqlite3_reset(m_stmt_update.get());
    sqlite3_bind_text(m_stmt_update.get(), 1, rel_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_update.get(), 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_update.get(), 3, mtime);

    if (sqlite3_step(m_stmt_update.get()) != SQLITE_DONE) {
        g_logger->error("[Database] Update 失败: {}", sqlite3_errmsg(m_db));
    }
}

void Database::remove_file(const std::string& rel_path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_delete) return;

    sqlite3_reset(m_stmt_delete.get());
    sqlite3_bind_text(m_stmt_delete.get(), 1, rel_path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_delete.get()) != SQLITE_DONE) {
        g_logger->error("[Database] Delete 失败: {}", sqlite3_errmsg(m_db));
    }
}
std::vector<std::string> Database::get_all_file_paths() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<std::string> paths;
    if (!m_db || !m_stmt_get_all_paths) return paths;

    sqlite3_reset(m_stmt_get_all_paths.get());
    while (sqlite3_step(m_stmt_get_all_paths.get()) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_all_paths.get(), 0));
        if (val) paths.emplace_back(val);
    }
    return paths;
}

void Database::begin_transaction() {
    // 这里依然需要 lock_guard，防止 begin 指令与其它 SQL 冲突
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
}

void Database::commit_transaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
}

void Database::rollback_transaction() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

// --- TransactionGuard 实现 ---

Database::TransactionGuard::TransactionGuard(Database& db) 
    : m_db(db), m_lock(db.m_mutex) { // 关键：在这里直接持有 unique_lock
    m_db.begin_transaction();
}

Database::TransactionGuard::~TransactionGuard() {
    if (!m_committed) {
        m_db.rollback_transaction();
    }
    // m_lock 析构时会自动解锁
}

void Database::TransactionGuard::commit() {
    if (!m_committed) {
        m_db.commit_transaction();
        m_committed = true;
    }
}

}  // namespace VeritasSync