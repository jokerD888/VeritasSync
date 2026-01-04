#include "VeritasSync/storage/StateManager.h"

#include <boost/asio.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <efsw/efsw.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <future>
#include <latch>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/P2PManager.h"

namespace VeritasSync {

    // UpdateListener 现在更简单了
    // 它只负责通知 StateManager 发生了变化，并处理防抖
    class UpdateListener : public efsw::FileWatchListener {
    public:
        // --- 构造函数现在接收 StateManager* ---
        UpdateListener(StateManager* owner)
            : m_owner(owner),
            m_debounce_timer(owner->get_io_context()) {
        }  // 从 owner 获取 io_context

        void handleFileAction(
            efsw::WatchID, const std::string& dir, const std::string& filename,
            efsw::Action action,
            std::string oldFilename) override {  // <-- 启用 oldFilename
            if (filename == "." || filename == "..") {
                return;
            }

            if (filename.find(".veritas.db") != std::string::npos) return;

            // 3. 忽略下载临时文件
            if (filename.find(".veritas_tmp") != std::string::npos) return;

            // 4. (可选) 忽略常见的系统垃圾文件，减少噪音
            if (filename == ".DS_Store" || filename == "Thumbs.db") return;

            if (action == efsw::Actions::Moved && !oldFilename.empty()) {
                // 拼接路径：Utf8ToPath 确保 dir(UTF-8) 和 filename(UTF-8) 被正确解析为 path
                std::filesystem::path old_file_path = Utf8ToPath(dir) / Utf8ToPath(oldFilename);

                // 转回 UTF-8 存入 map/数据库
                m_owner->notify_change_detected(PathToUtf8(old_file_path));
            }

            std::filesystem::path file_path = Utf8ToPath(dir) / Utf8ToPath(filename);

            // 1. 通知 StateManager 将变化暂存
            m_owner->notify_change_detected(PathToUtf8(file_path));
            // ---------------------------------------------------

            // 2. 重置防抖计时器
            m_debounce_timer.cancel();
            m_debounce_timer.expires_after(std::chrono::milliseconds(5000));

            // 3. 计时器触发后，调用 StateManager 的处理函数
            m_debounce_timer.async_wait([this](const boost::system::error_code& ec) {
                if (!ec) {
                    // 使用 post 确保在 io_context 线程上执行
                    boost::asio::post(m_owner->get_io_context(),
                        [this]() { m_owner->process_debounced_changes(); });
                }
                });
        }

    private:
        StateManager* m_owner;  // --- 持有 StateManager 指针 ---
        boost::asio::steady_timer m_debounce_timer;
    };

    // --- StateManager 实现 ---

    StateManager::StateManager(const std::string& root_path, P2PManager& p2p_manager, bool enable_watcher,
                               const std::string& sync_key)
        : m_root_path(std::filesystem::absolute(Utf8ToPath(root_path))),
          m_p2p_manager(&p2p_manager),
          m_sync_key(sync_key),
          m_retry_timer(nullptr) {
        if (!std::filesystem::exists(m_root_path)) {
            g_logger->info("[StateManager] 根目录 {} 不存在，正在创建。", PathToUtf8(m_root_path));
            std::error_code ec;
            std::filesystem::create_directory(m_root_path, ec);
            if (ec) {
                g_logger->error("[StateManager] ❌ 创建根目录失败: {} | {}", PathToUtf8(m_root_path), FormatErrorCode(ec));
            }
        }

        // --- 初始化数据库 ---
        // 数据库文件放在同步根目录下的隐藏文件中，例如 .veritas.db
        // 这样随目录移动，且不污染用户文件（已被默认 FileFilter 忽略）
        try {
            std::filesystem::path db_path = m_root_path / ".veritas.db";
            m_db = std::make_unique<Database>(db_path);
        } catch (const std::exception& e) {
            g_logger->error("[StateManager] 数据库初始化失败: {}", e.what());
        }
        // -------------------------

        if (enable_watcher) {
            // --- 初始加载忽略规则 ---
            m_file_filter.load_rules(m_root_path);

            // 初始化重试计时器
            m_retry_timer = std::make_unique<boost::asio::steady_timer>(get_io_context());

            m_file_watcher = std::make_unique<efsw::FileWatcher>();
            m_listener = std::make_unique<UpdateListener>(this);
            // true表示递归监控
            m_file_watcher->addWatch(m_root_path.string(), m_listener.get(), true);
            m_file_watcher->watch();        // 非阻塞监控
            g_logger->info("[StateManager] 已启动对目录 '{}' 的实时监控 (Source 模式)。", PathToUtf8(m_root_path));
        } else {
            g_logger->info("[StateManager] 以 'Destination' 模式启动，文件监控已禁用。");
        }
    }

    StateManager::~StateManager() {
        if (m_file_watcher) {
            g_logger->info("[StateManager] 正在停止文件监控...");
        }
        
        // --- 关键：安全关闭异步任务 ---
        if (m_retry_timer) {
            m_retry_timer->cancel();
        }
    }
    std::string StateManager::get_base_hash(const std::string& peer_id, const std::string& path) {
        if (!m_db) return "";
        // 复用上一阶段实现的 get_last_sent_hash，逻辑是一样的：查询 sync_history 表
        return m_db->get_last_sent_hash(peer_id, path);
    }

    void StateManager::record_sync_success(const std::string& peer_id, const std::string& path,
                                           const std::string& hash) {
        if (m_db) {
            m_db->update_sync_history(peer_id, path, hash);
        }
    }
    std::set<std::string> StateManager::get_local_directories() const {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        return m_dir_map;
    }

    void StateManager::add_dir_to_map(const std::string& relative_path) {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        m_dir_map.insert(relative_path);
    }

    void StateManager::remove_dir_from_map(const std::string& relative_path) {
        std::lock_guard<std::mutex> lock(m_dir_map_mutex);
        m_dir_map.erase(relative_path);
    }

    // --- 实现 get_io_context ---
    boost::asio::io_context& StateManager::get_io_context() {
        return m_p2p_manager->get_io_context();
    }

    // --- 实现 notify_change_detected (由 UpdateListener 调用) ---
    void StateManager::notify_change_detected(const std::string& full_path) {
        std::lock_guard<std::mutex> lock(m_changes_mutex);
        // insert 返回一个 pair<iterator, bool>，第二个值为 true 表示插入成功（即之前不存在）
        auto result = m_pending_changes.insert(full_path);

        if (result.second) {
            g_logger->debug("[{}] [Watcher] 检测到变化: {}", m_sync_key, full_path);
        }
    }

    // --- 由 UpdateListener 定时器调用 ---
    void StateManager::process_debounced_changes() {
        std::unordered_set<std::string> changes_to_process;
        {
            std::lock_guard<std::mutex> lock(m_changes_mutex);
            m_pending_changes.swap(changes_to_process);
        }

        if (changes_to_process.empty()) return;

        std::unordered_set<std::string> failed_changes;

        std::lock_guard<std::mutex> file_lock(m_file_map_mutex);
        std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);

        // --- 优化：按需重载规则 ---
        // 只有当变动列表中包含 .veritasignore 时才重载正则，避免昂贵的重复编译
        bool need_reload_filter = false;
        for (const auto& full_path_str : changes_to_process) {
            if (full_path_str.find(".veritasignore") != std::string::npos) {
                need_reload_filter = true;
                break;
            }
        }
        if (need_reload_filter) {
            g_logger->info("[StateManager] 检测到忽略规则变动，正在重载...");
            m_file_filter.load_rules(m_root_path);
        }

        // --- 开启事务 ---
        // 使用新实现的 TransactionGuard 替代旧的手动 begin/commit
        Database::TransactionGuard trans_guard(*m_db); 

        for (const auto& full_path_str : changes_to_process) {
            try {
                std::filesystem::path full_path = Utf8ToPath(full_path_str);

                if (full_path == m_root_path) continue;

                std::error_code ec;
                std::filesystem::path relative_path = std::filesystem::relative(full_path, m_root_path, ec);
                if (ec) {
                    g_logger->warn("[StateManager] 路径解析错误 {}: {}. 跳过。", full_path_str, ec.message());
                    continue;
                }

                const std::u8string u8_path_str = relative_path.generic_u8string(); // 👈 使用 generic 确保统一使用 '/'
                std::string rel_path_str(reinterpret_cast<const char*>(u8_path_str.c_str()), u8_path_str.length());

                if (m_file_filter.should_ignore(rel_path_str)) {
                    g_logger->debug("[{}] [Watcher] 忽略变更: {}", m_sync_key, rel_path_str);
                    continue;
                }

                if (std::filesystem::exists(full_path, ec) && !ec) {
                    if (std::filesystem::is_regular_file(full_path, ec) && !ec) {
                        // --- 文件更新 ---
                        FileInfo info;
                        info.path = rel_path_str;

                        auto ftime = std::filesystem::last_write_time(full_path, ec);
                        if (ec) {
                            g_logger->warn("[StateManager] 无法获取文件时间 (可能被锁定) {}: {}. 稍后重试。", rel_path_str, ec.message());
                            failed_changes.insert(full_path_str);
                            continue;
                        }
                        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
                        info.modified_time = sctp.time_since_epoch().count();

                        info.hash = Hashing::CalculateSHA256_MMIO(full_path);
                        if (info.hash.empty()) {
                            g_logger->warn("[StateManager] 哈希计算失败 (可能被锁定) {}. 稍后重试。", rel_path_str);
                            failed_changes.insert(full_path_str);
                            continue;
                        }
                        
                        // 【断点续传】获取文件大小
                        info.size = std::filesystem::file_size(full_path, ec);
                        if (ec) {
                            info.size = 0;  // 获取失败时设为 0
                        }

                        // 更新内存
                        m_file_map[info.path] = info;

                        // 更新数据库
                        if (m_db) m_db->update_file(info.path, info.hash, info.modified_time);

                        g_logger->info("[{}] [StateManager] 广播文件更新: {}", m_sync_key, info.path);
                        m_p2p_manager->broadcast_file_update(info);

                    } else if (std::filesystem::is_directory(full_path, ec) && !ec) {
                        if (m_dir_map.find(rel_path_str) == m_dir_map.end()) {
                            m_dir_map.insert(rel_path_str);
                            g_logger->info("[{}] [StateManager] 广播目录创建: {}", m_sync_key, rel_path_str);
                            m_p2p_manager->broadcast_dir_create(rel_path_str);
                        }
                    }
                } else {
                    // --- 文件删除 ---
                    if (m_file_map.erase(rel_path_str) > 0) {
                        // 从数据库删除
                        if (m_db) m_db->remove_file(rel_path_str);

                        g_logger->info("[{}] [StateManager] 广播文件删除: {}", m_sync_key, rel_path_str);
                        m_p2p_manager->broadcast_file_delete(rel_path_str);
                    } else if (m_dir_map.erase(rel_path_str) > 0) {
                        g_logger->info("[{}] [StateManager] 广播目录删除: {}", m_sync_key, rel_path_str);
                        m_p2p_manager->broadcast_dir_delete(rel_path_str);
                    }
                }
            } catch (const std::exception& e) {
                g_logger->error("[{}] [StateManager] 处理变更异常: {} ({}) - 将重试", m_sync_key, full_path_str, e.what());
                failed_changes.insert(full_path_str);
            }
        }

        // --- 提交事务 ---
        trans_guard.commit();

        // --- 处理失败重试 ---
        if (!failed_changes.empty()) {
            g_logger->info("[StateManager] {} 个文件处理失败，安排重试...", failed_changes.size());
            {
                std::lock_guard<std::mutex> lock(m_changes_mutex);
                for (const auto& path : failed_changes) {
                    m_pending_changes.insert(path);
                }
            }
            
            // 使用统一的线程安全重试接口
            schedule_retry(1);
        }
    }
    void StateManager::clear_sync_history(const std::string& path) {
        if (m_db) {
            m_db->remove_sync_history(path);
        }
    }
    std::optional<SyncHistory> StateManager::get_full_history(const std::string& peer_id, const std::string& path) {
        if (m_db) {
            return m_db->get_sync_history(peer_id, path);
        }
        return std::nullopt;
    }
    void StateManager::remove_path_from_map(const std::string& relative_path) {
        std::lock_guard<std::mutex> lock(m_file_map_mutex);
        m_file_map.erase(relative_path);

        // 同时也从数据库移除，保持一致性
        if (m_db) {
            m_db->remove_file(relative_path);
            m_db->remove_sync_history(relative_path);  // 移除“僵尸”同步历史！
        }
    }

    void StateManager::scan_directory() {
        g_logger->info("[StateManager] Scanning directory: {}", PathToUtf8(m_root_path));

        // 1. 加载忽略规则
        m_file_filter.load_rules(m_root_path);

        // Phase 1: 串行遍历目录，收集需要处理的文件列表
        // 这一步必须串行，因为 recursive_directory_iterator 不是线程安全的
        
        struct PendingFile {
            std::filesystem::path full_path;
            std::string rel_path_str;
            int64_t modified_time;
            std::string cached_hash;  // 如果有缓存命中，存储在这里
            bool need_calc;           // 是否需要计算哈希
            bool is_dirty = false;    // 👈 是否需要写回数据库
            uint64_t file_size = 0;   // 【断点续传】文件大小
        };
        
        std::vector<PendingFile> pending_files;
        std::set<std::string> pending_dirs;
        std::vector<std::string> retry_list;
        
        std::atomic<int> cache_hit_count{0};
        std::atomic<int> calc_count{0};
        
        {
            std::error_code ec;
            auto iterator = std::filesystem::recursive_directory_iterator(m_root_path, ec);
            
            if (ec) {
                g_logger->error("[StateManager] Error creating directory iterator: {}", ec.message());
                return;
            }
            
            for (const auto& entry : iterator) {
                std::filesystem::path relative_path = std::filesystem::relative(entry.path(), m_root_path, ec);
                if (ec) continue;

                // 3. 路径标准化
                // generic_u8string() 确保在 Windows 上也使用 '/'，实现跨平台一致性
                std::u8string u8_rel_path = relative_path.generic_u8string();
                std::string rel_path_str(reinterpret_cast<const char*>(u8_rel_path.c_str()), u8_rel_path.length());
                
                if (rel_path_str.empty()) continue;
                
                // 4. 子树修剪 (Subtree Pruning) - 关键性能优化
                if (m_file_filter.should_ignore(rel_path_str)) {
                    if (entry.is_directory(ec) && !ec) {
                        // 如果当前目录被忽略，直接跳过其内部所有文件的遍历
                        iterator.disable_recursion_pending();
                        g_logger->debug("[Scan] 修剪目录树: {}", rel_path_str);
                    }
                    continue;
                }
                
                if (entry.is_regular_file(ec) && !ec) {
                    auto ftime = std::filesystem::last_write_time(entry, ec);
                    if (ec) {
                        g_logger->warn("[Scan] 获取时间失败: {}", rel_path_str);
                        retry_list.push_back(entry.path().string());
                        continue;
                    }
                    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
                    int64_t mtime = sctp.time_since_epoch().count();
                    
                    // 检查数据库缓存
                    bool need_calc = true;
                    bool is_dirty = true;
                    std::string cached_hash;
                    if (m_db) {
                        auto cached_meta = m_db->get_file(rel_path_str);
                        if (cached_meta && cached_meta->mtime == mtime) {
                            cached_hash = cached_meta->hash;
                            need_calc = false;
                            is_dirty = false; // 命中了且时间戳一致，不脏
                            cache_hit_count++;
                        }
                    }
                    
                    // 【断点续传】获取文件大小
                    uint64_t fsize = entry.file_size(ec);
                    if (ec) fsize = 0;
                    
                    pending_files.push_back({
                        entry.path(),
                        std::move(rel_path_str),
                        mtime,
                        std::move(cached_hash),
                        need_calc,
                        is_dirty,
                        fsize
                    });
                    
                } else if (entry.is_directory(ec) && !ec) {
                    pending_dirs.insert(rel_path_str);
                }
            }
        }
        
        g_logger->info("[StateManager] Phase 1 完成: {} 个文件待处理, {} 个缓存命中", 
                      pending_files.size(), cache_hit_count.load());
        
        // Phase 2: 并行计算需要哈希的文件
        // 使用线程池 (boost::asio::thread_pool) 并行化计算，避免 std::async 的线程创建开销
        
        // 筛选需要计算哈希的文件
        std::vector<size_t> files_to_hash;
        for (size_t i = 0; i < pending_files.size(); ++i) {
            if (pending_files[i].need_calc) {
                files_to_hash.push_back(i);
            }
        }
        
        if (!files_to_hash.empty()) {
            g_logger->info("[StateManager] Phase 2: 并行计算 {} 个文件的哈希...", files_to_hash.size());
            
            // 确定并行度：使用硬件并发数，但不超过待处理文件数
            size_t num_threads = std::min(
                static_cast<size_t>(std::thread::hardware_concurrency()),
                files_to_hash.size()
            );
            num_threads = std::max(num_threads, size_t{1});
            
            // 创建线程池
            boost::asio::thread_pool hash_pool(num_threads);
            
            // 用于存储哈希结果的线程安全容器
            std::mutex result_mutex;
            std::vector<std::pair<size_t, std::string>> hash_results;
            hash_results.reserve(files_to_hash.size());
            
            // 用于追踪失败的文件
            std::mutex retry_mutex;
            
            for (size_t idx : files_to_hash) {
                boost::asio::post(hash_pool, [&, idx]() {
                    PendingFile& pf = pending_files[idx];
                    
                    // 使用 MMIO 版本计算大文件，标准版本计算小文件
                    // CalculateSHA256_MMIO 内部会自动判断
                    std::string hash = Hashing::CalculateSHA256_MMIO(pf.full_path);
                    
                    if (hash.empty()) {
                        g_logger->warn("[Scan] 哈希计算失败: {}", pf.rel_path_str);
                        std::lock_guard<std::mutex> lock(retry_mutex);
                        retry_list.push_back(pf.full_path.string());
                        return;
                    }
                    
                    // 存储结果
                    {
                        std::lock_guard<std::mutex> lock(result_mutex);
                        hash_results.emplace_back(idx, std::move(hash));
                    }
                    calc_count++;
                });
            }
            
            // 等待所有任务完成
            hash_pool.join();
            
            // 将哈希结果写回 pending_files
            for (auto& [idx, hash] : hash_results) {
                pending_files[idx].cached_hash = std::move(hash);
                pending_files[idx].need_calc = false;  // 标记已完成
            }
        }
        
        // Phase 3: 批量更新内存映射和数据库
        {
            std::lock_guard<std::mutex> file_lock(m_file_map_mutex);
            std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);
            
            m_file_map.clear();
            m_dir_map.clear();
            
            // 更新目录
            m_dir_map = std::move(pending_dirs);
            
            // 准备数据库批量更新
            std::vector<FileInfo> db_batch_updates;
            const size_t BATCH_SIZE = 500; // 调大窗口，平衡性能与锁定时间
            
            auto flush_to_db = [&]() {
                if (db_batch_updates.empty()) return;
                // 真正的窗口化提交：在这里开启事务，执行完立即提交释放锁
                Database::TransactionGuard window_guard(*m_db);
                for (const auto& f : db_batch_updates) {
                    m_db->update_file(f.path, f.hash, f.modified_time);
                }
                window_guard.commit();
                db_batch_updates.clear();
            };

            for (auto& pf : pending_files) {
                if (!pf.cached_hash.empty()) {
                    FileInfo info;
                    info.path = pf.rel_path_str;
                    info.hash = pf.cached_hash;
                    info.modified_time = pf.modified_time;
                    info.size = pf.file_size;  // 【断点续传】添加 size
                    
                    m_file_map[info.path] = info;
                    
                    if (pf.is_dirty) {
                        db_batch_updates.push_back(info);
                    }
                    
                    if (db_batch_updates.size() >= BATCH_SIZE) {
                        flush_to_db();
                    }
                }
            }
            // 提交最后一批不满 BATCH_SIZE 的数据
            flush_to_db();
        }
        
        // Phase 4: 清理数据库中的僵尸记录 (文件在磁盘已删但库里还有)
        if (m_db) {
            auto all_db_paths = m_db->get_all_file_paths();
            std::vector<std::string> paths_to_remove;
            
            {
                std::lock_guard<std::mutex> lock(m_file_map_mutex);
                for (const auto& db_path : all_db_paths) {
                    if (m_file_map.find(db_path) == m_file_map.end()) {
                        paths_to_remove.push_back(db_path);
                    }
                }
            }
            
            if (!paths_to_remove.empty()) {
                g_logger->info("[StateManager] 正在清理 {} 个数据库僵尸记录...", paths_to_remove.size());
                Database::TransactionGuard cleanup_guard(*m_db);
                for (const auto& p : paths_to_remove) {
                    m_db->remove_file(p);
                }
                cleanup_guard.commit();
            }
        }
        
        // Phase 5: 处理扫描期间失败的项目 (改为非阻塞异步重试)
        if (!retry_list.empty()) {
            g_logger->warn("[StateManager] 扫描发现 {} 个锁定文件，已安排异步重试。", retry_list.size());
            {
                std::lock_guard<std::mutex> lock(m_changes_mutex);
                for (const auto& path : retry_list) {
                    m_pending_changes.insert(path);
                }
            }
            
            // 启动重试计时器处理这些遗留项目 (统一接口)
            schedule_retry(2);
        }
        
        g_logger->info("[StateManager] 扫描完成。文件: {} (DB命中: {}, 重算: {})", 
                      m_file_map.size(), cache_hit_count.load(), calc_count.load());
    }

    std::string StateManager::get_state_as_json_string() {
        std::vector<FileInfo> files;
        {
            std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
            for (const auto& pair : m_file_map) {
                files.push_back(pair.second);
            }
        }
        nlohmann::json payload;
        payload["files"] = files;

        {
            std::lock_guard<std::mutex> lock(m_dir_map_mutex);
            payload["directories"] = m_dir_map;
        }

        nlohmann::json message;
        message[Protocol::MSG_TYPE] = Protocol::TYPE_SHARE_STATE;
        message[Protocol::MSG_PAYLOAD] = payload;
        return message.dump(2);
    }

    // --- 为 print_current_state 添加锁 ---
    void StateManager::print_current_state() const {
        std::lock_guard<std::mutex> lock(m_file_map_mutex);  // 锁定
        g_logger->info("--- Current Directory State ---");
        for (const auto& pair : m_file_map) {
            g_logger->info("  - Path: {}", pair.second.path);
            g_logger->info("    MTime: {}", pair.second.modified_time);
            g_logger->info("    Hash: {}...", pair.second.hash.substr(0, 12));
        }
        g_logger->info("-----------------------------");
    }

    bool StateManager::should_ignore_echo(const std::string& peer_id, const std::string& path,
                                          const std::string& remote_hash) {
        if (!m_db) return false;
        // 查库：我们上次发给这个人这个文件的 Hash 是多少？
        std::string last_sent = m_db->get_last_sent_hash(peer_id, path);

        // 如果不为空，且等于对方现在声称的 Hash -> 这是回声
        if (!last_sent.empty() && last_sent == remote_hash) {
            return true;
        }
        return false;
    }

    void StateManager::record_file_sent(const std::string& peer_id, const std::string& path, const std::string& hash) {
        if (m_db) {
            m_db->update_sync_history(peer_id, path, hash);
        }
    }

    std::string StateManager::get_file_hash(const std::string& relative_path) const {
        std::lock_guard<std::mutex> lock(m_file_map_mutex);
        auto it = m_file_map.find(relative_path);
        if (it != m_file_map.end()) {
            return it->second.hash;
        }
        return "";
    }

    std::vector<FileInfo> StateManager::get_all_files() const {
        std::lock_guard<std::mutex> lock(m_file_map_mutex);
        std::vector<FileInfo> files;
        files.reserve(m_file_map.size());
        for (const auto& [path, info] : m_file_map) {
            files.push_back(info);
        }
        return files;
    }

    void StateManager::schedule_retry(int delay_seconds) {
        if (!m_retry_timer) return;

        // 【核心加固】通过 post 确保所有的计时器操作都在 io_context 的线程内线性执行
        // 这样即使 scan_directory (主线程) 和之前挂起的回调同时触发重试，也不会发生竞态
        boost::asio::post(get_io_context(), [this, delay_seconds]() {
            if (!m_retry_timer) return;

            // 1. 设置超时：这会自动取消该计时器之前所有挂起的异步操作
            m_retry_timer->expires_after(std::chrono::seconds(delay_seconds));

            // 2. 重新注册异步等待
            m_retry_timer->async_wait([this](const boost::system::error_code& ec) {
                // 如果是正常超时响应 (不是被 cancel 的)
                if (!ec) {
                    this->process_debounced_changes();
                }
            });
        });
    }
}  // namespace VeritasSync
