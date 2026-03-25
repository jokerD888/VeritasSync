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
    std::unique_lock lock(m_mutex);
    m_cache[path] = FileMetadata{path, hash, mtime};
    return true;
}

bool CachedFileStore::remove(const std::string& path) {
    if (!m_db.remove_file(path)) {
        return false;
    }
    std::unique_lock lock(m_mutex);
    m_cache.erase(path);
    return true;
}

Database::TransactionGuard CachedFileStore::begin_transaction() {
    return Database::TransactionGuard(m_db);
}

size_t CachedFileStore::cache_size() const {
    std::shared_lock lock(m_mutex);
    return m_cache.size();
}

}  // namespace VeritasSync
