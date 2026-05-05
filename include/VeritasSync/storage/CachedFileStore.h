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
 * 事务安全：使用 thread_local 暂存区隔离多线程事务上下文。
 * 每个 CacheAwareGuard 持有独立的 m_local_pending_ops，
 * 通过静态线程局部指针 s_current_pending_ops 注册到当前线程。
 * 只有 commit 时才将暂存操作应用到全局缓存；事务析构则丢弃。
 *
 * RYOW (Read Your Own Writes)：事务内 get() 会倒序查询本事务的
 * 暂存区，保证读到自身未提交的写入，同时不泄漏给其他线程。
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
    // 事务内调用时，缓存更新暂存到 thread_local 暂存区；
    // 非事务时直接写缓存
    bool update(const std::string& path, const std::string& hash, int64_t mtime);
    bool remove(const std::string& path);

    /// 当前缓存条目数
    size_t cache_size() const;

private:
    friend class CacheAwareGuard;

    // 事务暂存操作类型（必须在 CacheAwareGuard 之前定义，因 Guard 成员包含 vector<PendingOp>）
    struct PendingOp {
        enum Type { Update, Remove };
        Type type;
        FileMetadata meta;  // Update: 完整 meta；Remove: meta.path 仅存路径用于 RYOW 查找
    };

    // 线程局部指针：指向当前线程正在执行的事务的暂存区
    // 无事务时为 nullptr
    static thread_local std::vector<PendingOp>* s_current_pending_ops;

public:
    /**
     * @brief 事务感知的 RAII Guard。
     *
     * 使用方式与 Database::TransactionGuard 相同：
     *   auto guard = store.begin_transaction();
     *   store.update(...);  // 缓存更新暂存（线程隔离）
     *   guard.commit();     // DB commit + 缓存生效
     *   // 析构时若未 commit → DB rollback + 丢弃缓存更新
     */
    class CacheAwareGuard {
    public:
        CacheAwareGuard(CachedFileStore& store);
        ~CacheAwareGuard();
        bool commit();  // 返回 false 表示落盘失败

        CacheAwareGuard(const CacheAwareGuard&) = delete;
        CacheAwareGuard& operator=(const CacheAwareGuard&) = delete;

    private:
        CachedFileStore& m_store;
        Database::TransactionGuard m_db_guard;
        std::vector<PendingOp> m_local_pending_ops;  // 本线程本事务的暂存区
        bool m_committed = false;
    };

    CacheAwareGuard begin_transaction();

private:

    Database& m_db;
    std::unordered_map<std::string, FileMetadata> m_cache;
    mutable std::shared_mutex m_mutex;
};

}  // namespace VeritasSync
