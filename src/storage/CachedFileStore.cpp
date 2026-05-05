#include "VeritasSync/storage/CachedFileStore.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// 定义 thread_local 静态成员
thread_local std::vector<CachedFileStore::PendingOp>* CachedFileStore::s_current_pending_ops = nullptr;

CachedFileStore::CachedFileStore(Database& db) : m_db(db) {}

void CachedFileStore::load_cache() {
    auto all_files = m_db.get_all_files();
    std::unique_lock lock(m_mutex);
    m_cache.clear();
    m_cache.reserve(all_files.size());
    for (auto& meta : all_files) {
        m_cache.emplace(meta.path, std::move(meta));
    }
}

std::optional<FileMetadata> CachedFileStore::get(const std::string& path) const {
    // RYOW：先查本线程事务的暂存区（倒序遍历找最新修改）
    if (s_current_pending_ops) {
        for (auto it = s_current_pending_ops->rbegin(); it != s_current_pending_ops->rend(); ++it) {
            if (it->meta.path == path) {
                if (it->type == PendingOp::Remove) return std::nullopt;
                return it->meta;
            }
        }
    }
    // 不在事务中或暂存区无命中，查全局稳态缓存
    std::shared_lock lock(m_mutex);
    auto cit = m_cache.find(path);
    if (cit != m_cache.end()) {
        return cit->second;
    }
    return std::nullopt;
}

std::vector<std::string> CachedFileStore::get_all_paths() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> paths;
    paths.reserve(m_cache.size());
    for (const auto& [path, _] : m_cache) {
        paths.push_back(path);
    }
    return paths;
}

bool CachedFileStore::update(const std::string& path, const std::string& hash, int64_t mtime) {
    if (!m_db.update_file(path, hash, mtime)) {
        return false;
    }
    if (s_current_pending_ops) {
        // 当前线程在事务中 → 暂存到线程局部 pending buffer
        // path 仅存于 meta.path 中，避免双重复制
        s_current_pending_ops->push_back({PendingOp::Update, FileMetadata{path, hash, mtime}});
    } else {
        std::unique_lock lock(m_mutex);
        m_cache.insert_or_assign(path, FileMetadata{path, hash, mtime});
    }
    return true;
}

bool CachedFileStore::remove(const std::string& path) {
    if (!m_db.remove_file(path)) {
        return false;
    }
    if (s_current_pending_ops) {
        // path 存入 meta.path 用于 RYOW 查找
        s_current_pending_ops->push_back({PendingOp::Remove, FileMetadata{path, "", 0}});
    } else {
        std::unique_lock lock(m_mutex);
        m_cache.erase(path);
    }
    return true;
}

size_t CachedFileStore::cache_size() const {
    std::shared_lock lock(m_mutex);
    return m_cache.size();
}

// --- CacheAwareGuard ---

CachedFileStore::CacheAwareGuard::CacheAwareGuard(CachedFileStore& store)
    : m_store(store), m_db_guard(store.m_db) {
    // 注册本 Guard 的暂存区到当前线程
    s_current_pending_ops = &m_local_pending_ops;
}

CachedFileStore::CacheAwareGuard::~CacheAwareGuard() {
    // DB 的 TransactionGuard 析构会自动 rollback
    // 解除当前线程的暂存区注册，m_local_pending_ops 随 Guard 析构自动丢弃
    s_current_pending_ops = nullptr;
}

bool CachedFileStore::CacheAwareGuard::commit() {
    if (m_committed) return true;
    if (!m_db_guard.commit()) {
        g_logger->error("[CachedFileStore] 事务提交失败");
        return false;
    }
    // DB 成功后，将暂存操作应用到全局缓存
    std::unique_lock lock(m_store.m_mutex);
    for (auto& op : m_local_pending_ops) {
        if (op.type == PendingOp::Update) {
            m_store.m_cache.insert_or_assign(op.meta.path, std::move(op.meta));
        } else {
            m_store.m_cache.erase(op.meta.path);
        }
    }
    m_committed = true;
    return true;
}

CachedFileStore::CacheAwareGuard CachedFileStore::begin_transaction() {
    return CacheAwareGuard(*this);
}

}  // namespace VeritasSync
