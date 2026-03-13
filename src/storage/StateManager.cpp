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

namespace VeritasSync {

    // ═══════════════════════════════════════════════════════════════
    // 阈值触发配置
    // ═══════════════════════════════════════════════════════════════
    
    // 批量触发阈值：当待处理变更数量达到此值时立即触发处理
    static constexpr size_t BATCH_TRIGGER_THRESHOLD = 100;
    
    // 最小间隔：两次批量处理之间的最小间隔（毫秒），避免过于频繁触发
    static constexpr int64_t MIN_BATCH_INTERVAL_MS = 1000;

    // 防抖定时器延迟（毫秒）
    static constexpr int DEBOUNCE_DELAY_MS = 5000;

    // UpdateListener 现在更简单了
    // 它只负责通知 StateManager 发生了变化，并处理防抖
    class UpdateListener : public efsw::FileWatchListener {
    public:
        // --- 构造函数现在接收 StateManager* ---
        UpdateListener(StateManager* owner)
            : m_owner(owner),
            m_debounce_timer(owner->m_io_context) {
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

            // 2. 检查是否需要立即触发批处理（非阻塞）
            bool triggered = m_owner->check_and_trigger_batch();
            
            // 3. 如果没有触发阈值，则使用正常的防抖定时器
            if (!triggered) {
                m_debounce_timer.cancel();
                m_debounce_timer.expires_after(std::chrono::milliseconds(DEBOUNCE_DELAY_MS));

                // 计时器触发后，调用 StateManager 的处理函数
                m_debounce_timer.async_wait([this](const boost::system::error_code& ec) {
                    if (!ec) {
                        // 使用 post 确保在 io_context 线程上执行
                        boost::asio::post(m_owner->m_io_context,
                            [this]() { m_owner->process_debounced_changes(); });
                    }
                });
            }
        }

    private:
        StateManager* m_owner;  // --- 持有 StateManager 指针 ---
        boost::asio::steady_timer m_debounce_timer;
    };

    // --- StateManager 实现 ---

    StateManager::StateManager(const std::string& root_path, boost::asio::io_context& io_context,
                               StateManagerCallbacks callbacks, bool enable_watcher,
                               const std::string& sync_key)
        : m_root_path(std::filesystem::absolute(Utf8ToPath(root_path))),
          m_io_context(io_context),
          m_callbacks(std::move(callbacks)),
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
            m_retry_timer = std::make_unique<boost::asio::steady_timer>(m_io_context);

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

    // --- 实现 notify_change_detected (由 UpdateListener 调用) ---
    void StateManager::notify_change_detected(const std::string& full_path) {
        std::lock_guard<std::mutex> lock(m_changes_mutex);
        // insert 返回一个 pair<iterator, bool>，第二个值为 true 表示插入成功（即之前不存在）
        auto result = m_pending_changes.insert(full_path);

        if (result.second) {
            g_logger->debug("[{}] [Watcher] 检测到变化: {}", m_sync_key, full_path);
        }
    }
    
    // --- 检查是否需要立即触发批处理（阈值触发机制）---
    // 关键：此函数在文件监控线程中调用，必须保持非阻塞
    bool StateManager::check_and_trigger_batch() {
        size_t pending_count = 0;
        {
            std::lock_guard<std::mutex> lock(m_changes_mutex);
            pending_count = m_pending_changes.size();
        }
        
        // 未达到阈值
        if (pending_count < BATCH_TRIGGER_THRESHOLD) {
            return false;
        }
        
        // 检查时间间隔
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_last_batch_time).count();
        
        if (elapsed_ms < MIN_BATCH_INTERVAL_MS) {
            return false;  // 间隔太短，等待下一次
        }
        
        // 检查是否正在处理中
        bool expected = false;
        if (!m_processing.compare_exchange_strong(expected, true)) {
            return false;  // 已经在处理中
        }
        
        g_logger->info("[{}] [StateManager] 阈值触发: {} 个变更待处理 (阈值={})", 
                      m_sync_key, pending_count, BATCH_TRIGGER_THRESHOLD);
        
        // 非阻塞：post 到 io_context，不在监控线程中执行耗时操作
        boost::asio::post(m_io_context, [this]() {
            process_debounced_changes();
            
            // 更新时间戳并释放处理标志
            m_last_batch_time = std::chrono::steady_clock::now();
            m_processing.store(false);
        });
        
        return true;
    }

    // --- 由 UpdateListener 定时器调用 ---
    // B-1 锁粒度优化：将 I/O、哈希计算、数据库操作移到锁外执行，
    // 仅在更新内存映射时短暂持锁，避免秒级阻塞其他线程。
    void StateManager::process_debounced_changes() {
        std::unordered_set<std::string> changes_to_process;
        {
            std::lock_guard<std::mutex> lock(m_changes_mutex);
            m_pending_changes.swap(changes_to_process);
        }

        if (changes_to_process.empty()) return;

        std::unordered_set<std::string> failed_changes;

        // --- 优化：按需重载规则（无锁，仅操作 m_file_filter）---
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

        // ═══════════════════════════════════════════════════════════
        // Phase 1（无锁）：执行所有 I/O 密集操作 — 路径解析、文件属性读取、
        //   SHA256 哈希计算。结果收集到临时容器中。
        // ═══════════════════════════════════════════════════════════

        // 中间结果容器
        struct FileUpdateResult {
            FileInfo info;
            bool is_echo = false;  // 回声标记（需要在锁内判定）
        };

        struct DeleteCheckResult {
            std::string rel_path;
            enum Type { Unknown, FileDelete, DirDelete } type = Unknown;
        };

        struct DirCreateResult {
            std::string rel_path;
        };

        std::vector<FileUpdateResult> file_update_results;
        std::vector<DeleteCheckResult> delete_check_results;
        std::vector<DirCreateResult> dir_create_results;

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

                const std::u8string u8_path_str = relative_path.generic_u8string();
                std::string rel_path_str(reinterpret_cast<const char*>(u8_path_str.c_str()), u8_path_str.length());

                if (m_file_filter.should_ignore(rel_path_str)) {
                    g_logger->debug("[{}] [Watcher] 忽略变更: {}", m_sync_key, rel_path_str);
                    continue;
                }

                if (std::filesystem::exists(full_path, ec) && !ec) {
                    if (std::filesystem::is_regular_file(full_path, ec) && !ec) {
                        // --- 文件更新：无锁执行 I/O 和哈希 ---
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

                        // ⑥ mtime 预检：如果数据库中已有该文件且 mtime 未变，
                        // 直接复用缓存的 hash，跳过昂贵的 SHA256 计算。
                        // （与 scan_directory 中的缓存策略一致）
                        bool need_hash = true;
                        if (m_db) {
                            auto cached = m_db->get_file(rel_path_str);
                            if (cached && static_cast<uint64_t>(cached->mtime) == info.modified_time) {
                                info.hash = cached->hash;
                                need_hash = false;
                                g_logger->debug("[{}] [StateManager] mtime 未变，跳过哈希: {}", m_sync_key, rel_path_str);
                            }
                        }

                        if (need_hash) {
                            info.hash = Hashing::CalculateSHA256_MMIO(full_path);
                            if (info.hash.empty()) {
                                g_logger->warn("[StateManager] 哈希计算失败 (可能被锁定) {}. 稍后重试。", rel_path_str);
                                failed_changes.insert(full_path_str);
                                continue;
                            }
                        }

                        // 【断点续传】获取文件大小
                        info.size = std::filesystem::file_size(full_path, ec);
                        if (ec) {
                            info.size = 0;
                        }

                        file_update_results.push_back({std::move(info), false});

                    } else if (std::filesystem::is_directory(full_path, ec) && !ec) {
                        dir_create_results.push_back({std::move(rel_path_str)});
                    }
                } else {
                    // --- 文件/目录删除：先记录，锁内判定类型 ---
                    delete_check_results.push_back({std::move(rel_path_str), DeleteCheckResult::Unknown});
                }
            } catch (const std::exception& e) {
                g_logger->error("[{}] [StateManager] 处理变更异常: {} ({}) - 将重试", m_sync_key, full_path_str, e.what());
                failed_changes.insert(full_path_str);
            }
        }

        // ═══════════════════════════════════════════════════════════
        // Phase 2（短锁）：批量更新内存映射（m_file_map / m_dir_map）
        //   仅执行内存操作，不做 I/O。
        // ═══════════════════════════════════════════════════════════

        // 批量广播收集容器
        std::vector<FileInfo> file_updates;
        std::vector<std::string> file_deletes;
        std::vector<std::string> dir_creates;
        std::vector<std::string> dir_deletes;

        {
            std::lock_guard<std::mutex> file_lock(m_file_map_mutex);
            std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);

            // 处理文件更新
            for (auto& result : file_update_results) {
                m_file_map[result.info.path] = result.info;

                // 【源头抑制】检查是否为接收文件的回声
                if (check_and_clear_echo(result.info.path, result.info.hash)) {
                    result.is_echo = true;
                    continue;
                }

                file_updates.push_back(result.info);
                g_logger->debug("[{}] [StateManager] 收集文件更新: {}", m_sync_key, result.info.path);
            }

            // 处理目录创建
            for (const auto& result : dir_create_results) {
                if (m_dir_map.find(result.rel_path) == m_dir_map.end()) {
                    m_dir_map.insert(result.rel_path);
                    dir_creates.push_back(result.rel_path);
                    g_logger->debug("[{}] [StateManager] 收集目录创建: {}", m_sync_key, result.rel_path);
                }
            }

            // 处理文件/目录删除（需要在锁内查询归属）
            for (auto& result : delete_check_results) {
                if (m_file_map.erase(result.rel_path) > 0) {
                    result.type = DeleteCheckResult::FileDelete;
                    file_deletes.push_back(result.rel_path);
                    g_logger->debug("[{}] [StateManager] 收集文件删除: {}", m_sync_key, result.rel_path);
                } else if (m_dir_map.erase(result.rel_path) > 0) {
                    result.type = DeleteCheckResult::DirDelete;
                    dir_deletes.push_back(result.rel_path);
                    g_logger->debug("[{}] [StateManager] 收集目录删除: {}", m_sync_key, result.rel_path);
                }
            }
        }
        // --- 锁已释放 ---

        // ═══════════════════════════════════════════════════════════
        // Phase 3（无锁）：数据库持久化 + 触发回调
        // ═══════════════════════════════════════════════════════════

        if (m_db) {
            Database::TransactionGuard trans_guard(*m_db);
            bool db_ok = true;

            // 写入文件更新（包括回声文件，也需要持久化到数据库）
            for (const auto& result : file_update_results) {
                if (!m_db->update_file(result.info.path, result.info.hash, result.info.modified_time)) {
                    g_logger->error("[{}] [StateManager] 持久化文件更新失败: {}", m_sync_key, result.info.path);
                    db_ok = false;
                    break;
                }
            }

            // 删除已移除的文件
            if (db_ok) {
                for (const auto& result : delete_check_results) {
                    if (result.type == DeleteCheckResult::FileDelete) {
                        if (!m_db->remove_file(result.rel_path)) {
                            g_logger->error("[{}] [StateManager] 持久化文件删除失败: {}", m_sync_key, result.rel_path);
                            db_ok = false;
                            break;
                        }
                    }
                }
            }

            if (db_ok) {
                trans_guard.commit();
            }
            // db_ok == false 时 TransactionGuard 析构自动 rollback
        }

        // 【解耦】通过回调通知外部变更，StateManager 不再知道 P2PManager 的存在
        if (!file_updates.empty()) {
            g_logger->info("[{}] [StateManager] 批量广播 {} 个文件更新", m_sync_key, file_updates.size());
            if (m_callbacks.on_file_updates) {
                m_callbacks.on_file_updates(file_updates);
            }
        }
        
        if (!file_deletes.empty()) {
            g_logger->info("[{}] [StateManager] 批量广播 {} 个文件删除", m_sync_key, file_deletes.size());
            if (m_callbacks.on_file_deletes) {
                m_callbacks.on_file_deletes(file_deletes);
            }
        }
        
        if (!dir_creates.empty() || !dir_deletes.empty()) {
            g_logger->info("[{}] [StateManager] 批量广播目录变更: {} 创建, {} 删除", 
                          m_sync_key, dir_creates.size(), dir_deletes.size());
            if (m_callbacks.on_dir_changes) {
                m_callbacks.on_dir_changes(dir_creates, dir_deletes);
            }
        }

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
        
        // Phase 3a（短锁）：仅更新内存映射，收集需要写数据库的脏数据
        // B-2 锁粒度优化：数据库事务移到锁外执行
        // 【修复 #6】先在锁外构建临时 map，再原子替换，避免 clear() 后其他线程读到空 map
        std::vector<FileInfo> db_dirty_entries;
        {
            // 在锁外预先构建新的 map
            std::unordered_map<std::string, FileInfo> new_file_map;
            for (auto& pf : pending_files) {
                if (!pf.cached_hash.empty()) {
                    FileInfo info;
                    info.path = pf.rel_path_str;
                    info.hash = pf.cached_hash;
                    info.modified_time = pf.modified_time;
                    info.size = pf.file_size;  // 【断点续传】添加 size
                    
                    new_file_map[info.path] = info;
                    
                    if (pf.is_dirty) {
                        db_dirty_entries.push_back(info);
                    }
                }
            }

            // 短暂锁内原子替换
            std::lock_guard<std::mutex> file_lock(m_file_map_mutex);
            std::lock_guard<std::mutex> dir_lock(m_dir_map_mutex);
            m_file_map = std::move(new_file_map);
            m_dir_map = std::move(pending_dirs);
        }
        // --- 锁已释放 ---
        
        // Phase 3b（无锁）：数据库批量写入
        if (m_db && !db_dirty_entries.empty()) {
            constexpr size_t DB_BATCH_WRITE_SIZE = 500;
            
            for (size_t i = 0; i < db_dirty_entries.size(); i += DB_BATCH_WRITE_SIZE) {
                size_t end = std::min(i + DB_BATCH_WRITE_SIZE, db_dirty_entries.size());
                Database::TransactionGuard window_guard(*m_db);
                bool window_ok = true;
                for (size_t j = i; j < end; ++j) {
                    const auto& f = db_dirty_entries[j];
                    if (!m_db->update_file(f.path, f.hash, f.modified_time)) {
                        g_logger->error("[StateManager] scan_directory 批量写入失败: {}", f.path);
                        window_ok = false;
                        break;
                    }
                }
                if (window_ok) {
                    window_guard.commit();
                }
            }
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
                bool cleanup_ok = true;
                for (const auto& p : paths_to_remove) {
                    if (!m_db->remove_file(p)) {
                        g_logger->error("[StateManager] 清理僵尸记录失败: {}", p);
                        cleanup_ok = false;
                        break;
                    }
                }
                if (cleanup_ok) {
                    cleanup_guard.commit();
                }
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
        boost::asio::post(m_io_context, [this, delay_seconds]() {
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

    // ═══════════════════════════════════════════════════════════════
    // 回声抑制（源头抑制）
    // ═══════════════════════════════════════════════════════════════

    void StateManager::mark_file_received(const std::string& path, const std::string& hash) {
        std::lock_guard<std::mutex> lock(m_received_files_mutex);
        m_received_files[path] = hash;
        g_logger->debug("[Echo] 记录接收文件: {} (hash: {}...)", path, hash.substr(0, 8));
    }

    bool StateManager::check_and_clear_echo(const std::string& path, const std::string& hash) {
        std::lock_guard<std::mutex> lock(m_received_files_mutex);
        auto it = m_received_files.find(path);
        if (it != m_received_files.end() && it->second == hash) {
            m_received_files.erase(it);
            g_logger->debug("[Echo] 检测到回声，跳过广播: {} (hash: {}...)", path, hash.substr(0, 8));
            return true;
        }
        return false;
    }
}  // namespace VeritasSync
