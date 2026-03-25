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
    bool update(const std::string& path, const std::string& hash, int64_t mtime);
    bool remove(const std::string& path);

    // --- 事务支持（透传底层 Database 的 TransactionGuard）---
    Database::TransactionGuard begin_transaction();

    /// 当前缓存条目数
    size_t cache_size() const;

private:
    Database& m_db;
    std::unordered_map<std::string, FileMetadata> m_cache;
    mutable std::shared_mutex m_mutex;
};

}  // namespace VeritasSync
