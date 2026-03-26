#pragma once

#include "VeritasSync/storage/Database.h"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VeritasSync {

/**
 * @brief Write-Through 缓存层，封装 Database 的文件元数据操作。
 *
 * 所有写操作先持久化到 DB，成功后自动更新内存缓存。
 * 调用方无需手动同步缓存，从结构上消除遗漏的可能性。
 *
 * 事务安全：在事务期间，缓存更新暂存在 pending buffer 中，
 * 只有 commit 时才应用到缓存；rollback 则丢弃。
 *
 * 线程安全：内部使用 shared_mutex（读并发、写互斥）。
 */
class CachedFileStore {
public:
    /// db 的生命周期必须长于 CachedFileStore
    explicit CachedFileStore(Database& db);

    /// 启动时批量加载数据库中所有文件元数据到内存缓存
    void load_cache();

    // --- 读操作（cache-first，shared_lock 并发读）---
    std::optional<FileMetadata> get(const std::string& path) const;
    std::vector<std::string> get_all_paths() const;

    // --- 写操作（write-through：先 DB 再 cache）---
    // 事务内调用时，缓存更新暂存；非事务时直接写缓存
    bool update(const std::string& path, const std::string& hash, int64_t mtime);
    bool remove(const std::string& path);

    /// 当前缓存条目数
    size_t cache_size() const;

    /**
     * @brief 事务感知的 RAII Guard。
     *
     * 使用方式与 Database::TransactionGuard 相同：
     *   auto guard = store.begin_transaction();
     *   store.update(...);  // 缓存更新暂存
     *   guard.commit();     // DB commit + 缓存生效
     *   // 析构时若未 commit → DB rollback + 丢弃缓存更新
     */
    class CacheAwareGuard {
    public:
        CacheAwareGuard(CachedFileStore& store);
        ~CacheAwareGuard();
        void commit();

        CacheAwareGuard(const CacheAwareGuard&) = delete;
        CacheAwareGuard& operator=(const CacheAwareGuard&) = delete;

    private:
        CachedFileStore& m_store;
        Database::TransactionGuard m_db_guard;
        bool m_committed = false;
    };

    CacheAwareGuard begin_transaction();

private:
    friend class CacheAwareGuard;

    void apply_pending();
    void discard_pending();

    // 事务状态（非线程安全：所有事务操作必须在同一线程中执行，
    // 当前由 StateManager 的 io_context/worker 线程保证）
    struct PendingOp {
        enum Type { Update, Remove };
        Type type;
        std::string path;
        FileMetadata meta;  // 仅 Update 时使用
    };

    Database& m_db;
    std::unordered_map<std::string, FileMetadata> m_cache;
    mutable std::shared_mutex m_mutex;

    // 事务期间暂存的缓存操作
    std::vector<PendingOp> m_pending_ops;
    bool m_in_transaction = false;
};

}  // namespace VeritasSync
