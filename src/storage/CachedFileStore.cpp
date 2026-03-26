#include "VeritasSync/storage/CachedFileStore.h"
#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

CachedFileStore::CachedFileStore(Database& db) : m_db(db) {}

void CachedFileStore::load_cache() {
    auto all_files = m_db.get_all_files();
    std::unique_lock lock(m_mutex);
    m_cache.clear();
    m_cache.reserve(all_files.size());
    for (auto& meta : all_files) {
        m_cache[meta.path] = std::move(meta);
    }
}

std::optional<FileMetadata> CachedFileStore::get(const std::string& path) const {
    std::shared_lock lock(m_mutex);
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        return it->second;
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
    if (m_in_transaction) {
        // 【安全修复 C4】事务内暂存缓存操作，commit 时才生效
        m_pending_ops.push_back({PendingOp::Update, path, FileMetadata{path, hash, mtime}});
    } else {
        std::unique_lock lock(m_mutex);
        m_cache[path] = FileMetadata{path, hash, mtime};
    }
    return true;
}

bool CachedFileStore::remove(const std::string& path) {
    if (!m_db.remove_file(path)) {
        return false;
    }
    if (m_in_transaction) {
        m_pending_ops.push_back({PendingOp::Remove, path, {}});
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

void CachedFileStore::apply_pending() {
    std::unique_lock lock(m_mutex);
    for (auto& op : m_pending_ops) {
        if (op.type == PendingOp::Update) {
            m_cache[op.path] = std::move(op.meta);
        } else {
            m_cache.erase(op.path);
        }
    }
    m_pending_ops.clear();
    m_in_transaction = false;
}

void CachedFileStore::discard_pending() {
    m_pending_ops.clear();
    m_in_transaction = false;
}

// --- CacheAwareGuard ---

CachedFileStore::CacheAwareGuard::CacheAwareGuard(CachedFileStore& store)
    : m_store(store), m_db_guard(store.m_db) {
    m_store.m_in_transaction = true;
    m_store.m_pending_ops.clear();
}

CachedFileStore::CacheAwareGuard::~CacheAwareGuard() {
    if (!m_committed) {
        // DB 的 TransactionGuard 析构会自动 rollback
        m_store.discard_pending();
    }
}

void CachedFileStore::CacheAwareGuard::commit() {
    if (!m_committed) {
        m_db_guard.commit();       // 先 commit DB
        m_store.apply_pending();   // 再应用缓存
        m_committed = true;
    }
}

CachedFileStore::CacheAwareGuard CachedFileStore::begin_transaction() {
    return CacheAwareGuard(*this);
}

}  // namespace VeritasSync
