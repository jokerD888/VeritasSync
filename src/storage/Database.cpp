#include "VeritasSync/storage/Database.h"

#include <sqlite3.h>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// --- StmtDeleter 实现 ---
void Database::StmtDeleter::operator()(sqlite3_stmt* s) {
    if (s) {
        sqlite3_finalize(s);
    }
}

Database::Database(const std::filesystem::path& db_path, int busy_timeout_ms) : m_db_path(PathToUtf8(db_path)) {
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
        g_logger->error("[Database] 无法打开数据库 {}: {}", m_db_path, 
                        m_db ? sqlite3_errmsg(m_db) : "未知错误");
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return;
    }

    // --- 设置繁忙重试超时 (5秒) ---
    // 防止多线程写入时出现 "database is locked" 错误
    sqlite3_busy_timeout(m_db, busy_timeout_ms);

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

    // --- 性能优化 PRAGMA ---
    // 内存映射 I/O：省去 read() 系统调用和内核态→用户态 DMA 拷贝，上限 256MB
    sqlite3_exec(m_db, "PRAGMA mmap_size=268435456;", nullptr, nullptr, nullptr);
    // 扩大 page cache 至 20MB，减少并发批量写入时的磁盘 I/O
    sqlite3_exec(m_db, "PRAGMA cache_size=-20000;", nullptr, nullptr, nullptr);
    // 临时表放内存，避免排序等操作生成磁盘文件
    sqlite3_exec(m_db, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);

    // 初始化表结构
    init_schema();
    // 预编译 SQL 语句
    prepare_statements();

    g_logger->info("[Database] 数据库已加载: {}", m_db_path);
}

Database::~Database() {
    if (m_db) {
        sqlite3_close_v2(m_db);  // v2 容忍未释放的 statement，避免 leaking
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

    // 每次 sqlite3_exec 后独立检查并释放 errMsg，防止泄漏
    auto exec_schema = [this](const char* sql, const char* description) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            g_logger->error("[Database] {} 失败: {}", description, errMsg ? errMsg : "unknown error");
            if (errMsg) sqlite3_free(errMsg);
        }
    };

    // 4. download_tasks 表（断点续传持久化）
    const char* sql_tasks =
        "CREATE TABLE IF NOT EXISTS download_tasks ("
        "  path TEXT PRIMARY KEY,"
        "  peer_id TEXT NOT NULL,"
        "  total_chunks INTEGER NOT NULL,"
        "  received_bitmap BLOB,"
        "  expected_hash TEXT,"
        "  expected_size INTEGER,"
        "  temp_path TEXT NOT NULL,"
        "  last_active INTEGER"
        ");";

    // 执行建表和索引创建
    exec_schema(sql_files, "创建 files 表");
    exec_schema(sql_history, "创建 sync_history 表");
    exec_schema(sql_index_path, "创建 sync_history path 索引");
    exec_schema(sql_tasks, "创建 download_tasks 表");
}

void Database::prepare_statements() {
    auto prepare = [this](const char* sql, ScopedStmt& stmt, const char* desc) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &raw, nullptr) != SQLITE_OK) {
            g_logger->error("[Database] 预编译失败 ({}): {}", desc, sqlite3_errmsg(m_db));
        }
        stmt.reset(raw);
    };

    prepare("SELECT hash, mtime FROM files WHERE path = ?;", m_stmt_get, "get_file");

    prepare("INSERT INTO files (path, hash, mtime) VALUES (?, ?, ?) "
            "ON CONFLICT(path) DO UPDATE SET hash=excluded.hash, mtime=excluded.mtime;",
            m_stmt_update, "update_file");

    prepare("DELETE FROM files WHERE path = ?;", m_stmt_delete, "delete_file");

    // 记录发送历史 (Upsert)
    prepare("INSERT INTO sync_history (peer_id, path, hash, ts) VALUES (?, ?, ?, STRFTIME('%s','now')) "
            "ON CONFLICT(peer_id, path) DO UPDATE SET hash=excluded.hash, ts=excluded.ts;",
            m_stmt_hist_update, "update_sync_history");

    prepare("SELECT hash, ts FROM sync_history WHERE peer_id = ? AND path = ?;",
            m_stmt_hist_get, "get_sync_history");

    prepare("DELETE FROM sync_history WHERE path = ?;",
            m_stmt_hist_delete, "delete_sync_history");

    prepare("SELECT path FROM files;", m_stmt_get_all_paths, "get_all_paths");

    prepare("SELECT path, hash, mtime FROM files;", m_stmt_get_all_files, "get_all_files");

    // 断点续传
    prepare("INSERT OR REPLACE INTO download_tasks "
            "(path, peer_id, total_chunks, received_bitmap, expected_hash, expected_size, temp_path, last_active) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?);",
            m_stmt_save_task, "save_download_task");

    prepare("DELETE FROM download_tasks WHERE path = ?;",
            m_stmt_remove_task, "remove_download_task");

    prepare("SELECT path, peer_id, total_chunks, received_bitmap, "
            "expected_hash, expected_size, temp_path, last_active "
            "FROM download_tasks;",
            m_stmt_load_all_tasks, "load_all_tasks");
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
bool Database::remove_sync_history(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_delete) return false;

    sqlite3_reset(m_stmt_hist_delete.get());
    sqlite3_bind_text(m_stmt_hist_delete.get(), 1, path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_hist_delete.get()) != SQLITE_DONE) {
        g_logger->error("[Database] 删除历史记录失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}
bool Database::update_sync_history(const std::string& peer_id, const std::string& path, const std::string& hash) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_hist_update) return false;

    sqlite3_reset(m_stmt_hist_update.get());
    sqlite3_bind_text(m_stmt_hist_update.get(), 1, peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update.get(), 2, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_hist_update.get(), 3, hash.c_str(), -1, SQLITE_STATIC);
    // 第 4 个参数 ts 已由 SQL 内置函数 STRFTIME 处理，无需 C++ 绑定

    if (sqlite3_step(m_stmt_hist_update.get()) != SQLITE_DONE) {
        g_logger->error("[Database] 更新同步历史失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
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
        const char* hash_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get.get(), 0));
        if (!hash_val) return std::nullopt;
        meta.hash = hash_val;
        meta.mtime = sqlite3_column_int64(m_stmt_get.get(), 1);
        return meta;
    }

    return std::nullopt;
}

bool Database::update_file(const std::string& rel_path, const std::string& hash, int64_t mtime) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_update) return false;

    sqlite3_reset(m_stmt_update.get());
    sqlite3_bind_text(m_stmt_update.get(), 1, rel_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_update.get(), 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_update.get(), 3, mtime);

    if (sqlite3_step(m_stmt_update.get()) != SQLITE_DONE) {
        g_logger->error("[Database] Update 失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool Database::remove_file(const std::string& rel_path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_delete) return false;

    sqlite3_reset(m_stmt_delete.get());
    sqlite3_bind_text(m_stmt_delete.get(), 1, rel_path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_delete.get()) != SQLITE_DONE) {
        g_logger->error("[Database] Delete 失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
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

std::vector<FileMetadata> Database::get_all_files() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<FileMetadata> files;
    if (!m_db || !m_stmt_get_all_files) return files;
    files.reserve(1024);  // 经验值预分配，避免双重全表扫描

    sqlite3_reset(m_stmt_get_all_files.get());
    while (sqlite3_step(m_stmt_get_all_files.get()) == SQLITE_ROW) {
        const char* path_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_all_files.get(), 0));
        const char* hash_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_get_all_files.get(), 1));
        if (path_val && hash_val) {
            FileMetadata meta;
            meta.path = path_val;
            meta.hash = hash_val;
            meta.mtime = sqlite3_column_int64(m_stmt_get_all_files.get(), 2);
            files.push_back(std::move(meta));
        }
    }
    return files;
}

// --- 无锁事务操作（供 TransactionGuard 直接调用）---

bool Database::begin_transaction_nolock() {
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        g_logger->error("[Database] BEGIN TRANSACTION 失败: {}", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::commit_transaction_nolock() {
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        g_logger->error("[Database] COMMIT 失败: {}", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Database::rollback_transaction_nolock() {
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        g_logger->error("[Database] ROLLBACK 失败: {}", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// --- 断点续传持久化 ---

bool Database::save_download_task(const DownloadTaskRecord& task) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_save_task) return false;

    sqlite3_reset(m_stmt_save_task.get());
    sqlite3_bind_text(m_stmt_save_task.get(), 1, task.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_save_task.get(), 2, task.peer_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_save_task.get(), 3, task.total_chunks);
    sqlite3_bind_blob(m_stmt_save_task.get(), 4,
                      task.bitmap_data.empty() ? nullptr : task.bitmap_data.data(),
                      static_cast<int>(task.bitmap_data.size()), SQLITE_STATIC);
    sqlite3_bind_text(m_stmt_save_task.get(), 5,
                      task.expected_hash.empty() ? nullptr : task.expected_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_save_task.get(), 6, static_cast<int64_t>(task.expected_size));
    sqlite3_bind_text(m_stmt_save_task.get(), 7, task.temp_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(m_stmt_save_task.get(), 8, task.last_active);

    if (sqlite3_step(m_stmt_save_task.get()) != SQLITE_DONE) {
        g_logger->error("[Database] save_download_task 失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool Database::remove_download_task(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db || !m_stmt_remove_task) return false;

    sqlite3_reset(m_stmt_remove_task.get());
    sqlite3_bind_text(m_stmt_remove_task.get(), 1, path.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(m_stmt_remove_task.get()) != SQLITE_DONE) {
        g_logger->error("[Database] remove_download_task 失败: {}", sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

std::vector<DownloadTaskRecord> Database::load_all_download_tasks() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<DownloadTaskRecord> tasks;
    if (!m_db || !m_stmt_load_all_tasks) return tasks;

    sqlite3_reset(m_stmt_load_all_tasks.get());
    while (sqlite3_step(m_stmt_load_all_tasks.get()) == SQLITE_ROW) {
        DownloadTaskRecord task;

        const char* path_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_load_all_tasks.get(), 0));
        if (!path_val) continue;
        task.path = path_val;

        const char* peer_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_load_all_tasks.get(), 1));
        if (peer_val) task.peer_id = peer_val;

        task.total_chunks = static_cast<uint32_t>(sqlite3_column_int64(m_stmt_load_all_tasks.get(), 2));

        // 读取 bitmap BLOB
        int blob_size = sqlite3_column_bytes(m_stmt_load_all_tasks.get(), 3);
        if (blob_size > 0) {
            const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(m_stmt_load_all_tasks.get(), 3));
            task.bitmap_data.assign(blob, blob + blob_size);
        }

        const char* hash_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_load_all_tasks.get(), 4));
        if (hash_val) task.expected_hash = hash_val;

        task.expected_size = static_cast<uint64_t>(sqlite3_column_int64(m_stmt_load_all_tasks.get(), 5));

        const char* temp_val = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt_load_all_tasks.get(), 6));
        if (temp_val) task.temp_path = temp_val;

        task.last_active = sqlite3_column_int64(m_stmt_load_all_tasks.get(), 7);

        tasks.push_back(std::move(task));
    }
    return tasks;
}

// --- TransactionGuard 实现 ---

Database::TransactionGuard::TransactionGuard(Database& db) 
    : m_db(db), m_lock(db.m_mutex) {
    // 直接调用无锁版本，避免通过公有方法重复加锁
    m_db.begin_transaction_nolock();
}

Database::TransactionGuard::~TransactionGuard() {
    if (!m_committed) {
        m_db.rollback_transaction_nolock();  // 析构中不抛异常
    }
    // m_lock 析构时自动解锁
}

bool Database::TransactionGuard::commit() {
    if (m_committed) return true;
    if (!m_db.commit_transaction_nolock()) return false;
    m_committed = true;
    return true;
}

}  // namespace VeritasSync