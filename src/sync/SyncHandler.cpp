#include "VeritasSync/sync/SyncHandler.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/SyncManager.h"

namespace VeritasSync {

SyncHandler::SyncHandler(StateManager* state_manager,
                         std::shared_ptr<TransferManager> transfer_manager,
                         boost::asio::thread_pool& worker_pool,
                         boost::asio::io_context& io_context,
                         SendToPeerFunc send_to_peer,
                         SendToPeerSafeFunc send_to_peer_safe,
                         WithPeerFunc with_peer)
    : m_state_manager(state_manager),
      m_transfer_manager(std::move(transfer_manager)),
      m_worker_pool(worker_pool),
      m_io_context(io_context),
      m_send_to_peer(std::move(send_to_peer)),
      m_send_to_peer_safe(std::move(send_to_peer_safe)),
      m_with_peer(std::move(with_peer)) {}

bool SyncHandler::can_receive() const {
    return m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional;
}

void SyncHandler::refresh_peer_timeout(PeerController* from_peer) {
    if (!from_peer) return;

    std::string pid = from_peer->get_peer_id();
    uint64_t sid = from_peer->sync_session_id.load();

    boost::asio::post(m_io_context, [this, pid, sid]() {
        m_with_peer(pid, [sid](PeerController* peer) {
            if (peer->sync_session_id.load() == sid && peer->sync_timeout_timer) {
                peer->sync_timeout_timer->expires_after(std::chrono::seconds(60));
            }
        });
    });
}

// ═══════════════════════════════════════════════════════════════
// handle_share_state
// ═══════════════════════════════════════════════════════════════

void SyncHandler::handle_share_state(const nlohmann::json& payload, PeerController* from_peer) {
    if (!can_receive()) return;

    std::string peer_id = from_peer ? from_peer->get_peer_id() : "";
    if (peer_id.empty()) return;

    int64_t safe_threshold_ts = from_peer ? (from_peer->connected_at_ts.load() - 5) : 0;

    g_logger->info("[KCP] (Destination) 收到来自 {} 的状态。连接TS: {}, 历史阈值: {}", peer_id,
                   from_peer->connected_at_ts.load(), safe_threshold_ts);

    boost::asio::post(m_worker_pool, [this, payload, peer_id, safe_threshold_ts]() {
        if (!m_state_manager) {
            g_logger->error("[Sync] StateManager 为空，无法处理状态。");
            return;
        }
        
        m_state_manager->scan_directory();
        
        std::vector<FileInfo> remote_files;
        std::set<std::string> remote_dirs;
        
        try {
            if (payload.contains("files")) {
                for (const auto& file_json : payload["files"]) {
                    FileInfo fi;
                    fi.path = file_json.value("path", "");
                    fi.modified_time = file_json.value("mtime", static_cast<uint64_t>(0));
                    fi.hash = file_json.value("hash", "");
                    fi.size = file_json.value("size", static_cast<uint64_t>(0));
                    if (!fi.path.empty()) {
                        remote_files.push_back(fi);
                    }
                }
            }
            if (payload.contains("directories")) {
                for (const auto& dir : payload["directories"]) {
                    remote_dirs.insert(dir.get<std::string>());
                }
            }
        } catch(const std::exception& e) {
            g_logger->error("[Sync] 解析远程状态失败: {}", e.what());
            return;
        }
        
        // 使用 SyncManager 进行比较
        auto get_history = [this, peer_id](const std::string& path) -> std::optional<SyncHistory> {
            return m_state_manager->get_full_history(peer_id, path);
        };
        
        SyncActions file_actions = SyncManager::compare_states_and_get_requests(
            m_state_manager->get_all_files(), remote_files, get_history, m_mode);
        DirSyncActions dir_actions = SyncManager::compare_dir_states(
            m_state_manager->get_local_directories(), remote_dirs, m_mode);

        // E1. 删除多余文件
        if (!file_actions.files_to_delete.empty()) {
            g_logger->info("[Sync] 计划删除 {} 个本地多余的文件。", file_actions.files_to_delete.size());
            for (const auto& file_path_str : file_actions.files_to_delete) {
                std::filesystem::path relative_path = Utf8ToPath(file_path_str);
                std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
                std::error_code ec;

                if (std::filesystem::remove(full_path, ec)) {
                    g_logger->info("[Sync] -> 已删除 (相对路径): {}", file_path_str);
                    m_state_manager->remove_path_from_map(file_path_str);
                } else if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", file_path_str, FormatErrorCode(ec));
                }
            }
        }

        // E2. 删除多余目录
        if (!dir_actions.dirs_to_delete.empty()) {
            std::vector<std::string> sorted_dirs = dir_actions.dirs_to_delete;
            std::sort(sorted_dirs.begin(), sorted_dirs.end(),
                      [](const std::string& a, const std::string& b) { return a.length() > b.length(); });

            for (const auto& dir_path_str : sorted_dirs) {
                std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                bool deleted = false;

                if (m_mode == SyncMode::OneWay) {
                    if (std::filesystem::remove_all(full_path, ec) != static_cast<std::uintmax_t>(-1)) {
                        deleted = true;
                    }
                } else {
                    if (std::filesystem::remove(full_path, ec)) {
                        deleted = true;
                    } else if (ec && ec != std::errc::directory_not_empty) {
                        g_logger->warn("[Sync] 删除目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                    }
                }

                if (deleted || (!deleted && !std::filesystem::exists(full_path))) {
                    m_state_manager->remove_dir_from_map(dir_path_str);
                }
            }
        }

        // E3. 创建缺失目录
        if (!dir_actions.dirs_to_create.empty()) {
            for (const auto& dir_path_str : dir_actions.dirs_to_create) {
                std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                std::filesystem::create_directories(full_path, ec);
                if (!ec) {
                    m_state_manager->add_dir_to_map(dir_path_str);
                } else {
                    g_logger->warn("[Sync] 创建目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                }
            }
        }

        // F. 发送文件请求
        if (!file_actions.files_to_request.empty()) {
            g_logger->info("[KCP] 计划向 {} 请求 {} 个缺失/过期的文件。", peer_id,
                           file_actions.files_to_request.size());
            
            // 构建文件路径到 FileInfo 的映射，用于获取 hash/size
            std::map<std::string, FileInfo> remote_file_map;
            for (const auto& fi : remote_files) {
                remote_file_map[fi.path] = fi;
            }

            boost::asio::post(m_io_context, [this, peer_id, 
                              reqs = std::move(file_actions.files_to_request),
                              remote_file_map = std::move(remote_file_map)]() {
                m_with_peer(peer_id, [this, &peer_id, &reqs, &remote_file_map](PeerController* peer_ctrl) {
                    if (!peer_ctrl || !peer_ctrl->is_connected()) return;
                    
                    for (const auto& file_path : reqs) {
                        nlohmann::json request_msg;
                        request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                        
                        // 【断点续传】获取远程文件信息
                        std::string remote_hash;
                        uint64_t remote_size = 0;
                        auto fit = remote_file_map.find(file_path);
                        if (fit != remote_file_map.end()) {
                            remote_hash = fit->second.hash;
                            remote_size = fit->second.size;
                        }
                        
                        // 【断点续传】检查是否可以续传
                        auto resume_info = m_transfer_manager->check_resume_eligibility(
                            file_path, remote_hash, remote_size);
                        
                        if (resume_info) {
                            request_msg[Protocol::MSG_PAYLOAD] = {
                                {"path", file_path},
                                {"start_chunk", resume_info->received_chunks},
                                {"expected_hash", resume_info->expected_hash},
                                {"expected_size", resume_info->expected_size}
                            };
                            g_logger->info("[P2P] 发送续传请求: {} 从 chunk #{} 开始", 
                                          file_path, resume_info->received_chunks);
                        } else {
                            m_transfer_manager->register_expected_metadata(
                                file_path, peer_id, remote_hash, remote_size);
                            request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
                        }
                        
                        m_send_to_peer(request_msg.dump(), peer_ctrl);
                    }
                });
            });
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// handle_file_update
// ═══════════════════════════════════════════════════════════════

void SyncHandler::handle_file_update(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;

    if (from_peer) {
        from_peer->received_file_count.fetch_add(1);
        refresh_peer_timeout(from_peer);
    }
    
    if (!m_state_manager) return;
    
    FileInfo remote_info;
    try {
        remote_info = payload.get<FileInfo>();
    } catch (const std::exception& e) {
        g_logger->error("[KCP] 解析 file_update 失败: {}", e.what());
        return;
    }

    std::string peer_id = from_peer ? from_peer->get_peer_id() : "";
    if (peer_id.empty()) return;

    // Offload 耗时操作到 Worker 线程
    boost::asio::post(m_worker_pool, [this, remote_info, peer_id]() {
        // --- 1. 拦截回声 (Echo Check) ---
        if (m_state_manager->should_ignore_echo(peer_id, remote_info.path, remote_info.hash)) {
            return;
        }

        g_logger->info("[P2P] 收到更新请求: {}", remote_info.path);

        std::filesystem::path relative_path = Utf8ToPath(remote_info.path);
        std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;

        bool should_request = false;
        std::error_code ec;

        // --- 2. 冲突检测 (Conflict Resolution) ---
        if (!std::filesystem::exists(full_path, ec)) {
            g_logger->info("[Sync] 本地缺失，准备下载: {}", remote_info.path);
            should_request = true;
        } else {
            std::string remote_hash = remote_info.hash;
            std::string local_hash = Hashing::CalculateSHA256(full_path);
            std::string base_hash = m_state_manager->get_base_hash(peer_id, remote_info.path);

            if (local_hash == remote_hash) {
                g_logger->info("[Sync] 内容一致，无需更新: {}", remote_info.path);
                m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                return;
            }

            bool local_changed = !base_hash.empty() && (local_hash != base_hash);
            bool remote_changed = !base_hash.empty() && (remote_hash != base_hash);

            if (base_hash.empty() || (!local_changed && remote_changed)) {
                g_logger->info("[Sync] 正常更新 (本地未修改): {}", remote_info.path);
                should_request = true;
                
            } else if (local_changed && !remote_changed) {
                g_logger->info("[Sync] 本地版本更新，忽略远程旧版本: {} (local={}, remote={}, base={})", 
                               remote_info.path,
                               local_hash.substr(0, 6),
                               remote_hash.substr(0, 6),
                               base_hash.substr(0, 6));
                m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                should_request = false;
                
            } else if (!local_changed && !remote_changed) {
                g_logger->debug("[Sync] 状态一致，无需操作: {}", remote_info.path);
                should_request = false;
                
            } else {
                // 情况 4: 双方都改了 → 真正的冲突
                g_logger->warn("[Sync] ⚠️ 检测到冲突 (双方都修改了): {}", remote_info.path);
                g_logger->warn("       Base: {}...", base_hash.substr(0, std::min<size_t>(6, base_hash.size())));
                g_logger->warn("       Local: {}...", local_hash.substr(0, std::min<size_t>(6, local_hash.size())));
                g_logger->warn("       Remote: {}...", remote_hash.substr(0, std::min<size_t>(6, remote_hash.size())));

                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

                std::string filename = relative_path.stem().string();
                std::string ext = relative_path.extension().string();
                std::string conflict_name = filename + ".conflict." + std::to_string(timestamp) + ext;
                std::filesystem::path conflict_path = full_path.parent_path() / conflict_name;

                std::error_code ren_ec;
                std::filesystem::rename(full_path, conflict_path, ren_ec);

                if (!ren_ec) {
                    g_logger->warn("[Sync] ⚡ 本地冲突文件已重命名为: {}", conflict_path.filename().string());
                    should_request = true;
                } else {
                    g_logger->error("[Sync] ❌ 冲突处理失败 (无法重命名): {} | {}", remote_info.path, FormatErrorCode(ren_ec));
                    return;
                }
            }
        }

        if (should_request) {
            nlohmann::json request_msg;
            request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
            
            auto resume_info = m_transfer_manager->check_resume_eligibility(
                remote_info.path, remote_info.hash, remote_info.size);
            
            if (resume_info) {
                request_msg[Protocol::MSG_PAYLOAD] = {
                    {"path", remote_info.path},
                    {"start_chunk", resume_info->received_chunks},
                    {"expected_hash", resume_info->expected_hash},
                    {"expected_size", resume_info->expected_size}
                };
                g_logger->info("[P2P] 发送续传请求: {} 从 chunk #{} 开始", 
                              remote_info.path, resume_info->received_chunks);
            } else {
                m_transfer_manager->register_expected_metadata(
                    remote_info.path, peer_id, remote_info.hash, remote_info.size);
                request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
            }
            
            std::string msg_str = request_msg.dump();
            boost::asio::post(m_io_context, [this, peer_id, msg_str]() {
                 m_send_to_peer_safe(msg_str, peer_id);
            });
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// handle_file_delete
// ═══════════════════════════════════════════════════════════════

void SyncHandler::handle_file_delete(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_file_count.fetch_add(1);
        refresh_peer_timeout(from_peer);
    }

    if (!m_state_manager) return;
    
    boost::asio::post(m_worker_pool, [this, payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 file_delete 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量删除: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        if (std::filesystem::remove(full_path, ec)) {
            g_logger->info("[Sync] -> 已删除本地文件: {}", relative_path_str);
            m_state_manager->remove_path_from_map(relative_path_str);
        } else {
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            } else {
                g_logger->debug("[Sync] 文件已不存在: {}", relative_path_str);
            }
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// handle_dir_create / handle_dir_delete
// ═══════════════════════════════════════════════════════════════

void SyncHandler::handle_dir_create(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_dir_count.fetch_add(1);
        refresh_peer_timeout(from_peer);
    }
    
    if (!m_state_manager) return;
    
    boost::asio::post(m_worker_pool, [this, payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 dir_create 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录创建: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        if (std::filesystem::create_directories(full_path, ec)) {
            g_logger->info("[Sync] -> 已创建目录: {}", relative_path_str);
            m_state_manager->add_dir_to_map(relative_path_str);
        } else if (ec) {
            g_logger->error("[Sync] ❌ 创建目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
        }
    });
}

void SyncHandler::handle_file_request(const nlohmann::json& /*payload*/, PeerController* /*from_peer*/) {
    // 文件请求由 TransferManager 处理，这里只是占位
}

void SyncHandler::handle_dir_delete(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    
    if (from_peer) {
        from_peer->received_dir_count.fetch_add(1);
        refresh_peer_timeout(from_peer);
    }
    
    if (!m_state_manager) return;
    
    boost::asio::post(m_worker_pool, [this, payload]() {
        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] 解析增量目录删除消息失败: {}", e.what());
            return;
        } catch (...) {
            g_logger->error("[KCP] 解析增量目录删除消息时发生未知异常");
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录删除: {}", relative_path_str);

        std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);

        std::error_code ec;
        std::filesystem::remove_all(full_path, ec);

        if (!ec) {
            g_logger->info("[Sync] -> 已删除目录: {}", relative_path_str);
            m_state_manager->remove_dir_from_map(relative_path_str);
        } else {
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] ❌ 删除目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            } else {
                m_state_manager->remove_dir_from_map(relative_path_str);
            }
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 批量消息处理器
// ═══════════════════════════════════════════════════════════════

void SyncHandler::handle_file_update_batch(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    if (!m_state_manager) return;
    
    std::vector<FileInfo> files;
    try {
        if (!payload.contains("files")) {
            g_logger->error("[KCP] file_update_batch 缺少 files 字段");
            return;
        }
        for (const auto& file_json : payload["files"]) {
            FileInfo fi;
            fi.path = file_json.value("path", "");
            fi.modified_time = file_json.value("mtime", static_cast<uint64_t>(0));
            fi.hash = file_json.value("hash", "");
            fi.size = file_json.value("size", static_cast<uint64_t>(0));
            if (!fi.path.empty()) {
                files.push_back(fi);
            }
        }
    } catch (const std::exception& e) {
        g_logger->error("[KCP] 解析 file_update_batch 失败: {}", e.what());
        return;
    }
    
    g_logger->info("[KCP] (Destination) 收到批量文件更新: {} 个文件", files.size());
    
    if (from_peer) {
        from_peer->received_file_count.fetch_add(static_cast<int>(files.size()));
        refresh_peer_timeout(from_peer);
    }
    
    std::string peer_id = from_peer ? from_peer->get_peer_id() : "";
    if (peer_id.empty()) return;
    
    boost::asio::post(m_worker_pool, [this, files = std::move(files), peer_id]() {
        try {
        std::vector<std::pair<FileInfo, bool>> files_to_request;
        
        for (const auto& remote_info : files) {
            // 1. 拦截回声
            if (m_state_manager->should_ignore_echo(peer_id, remote_info.path, remote_info.hash)) {
                continue;
            }

            g_logger->info("[P2P] 收到更新请求: {}", remote_info.path);

            std::filesystem::path relative_path = Utf8ToPath(remote_info.path);
            std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;

            bool should_request = false;
            std::error_code ec;

            // 2. 冲突检测
            if (!std::filesystem::exists(full_path, ec)) {
                g_logger->info("[Sync] 本地缺失，准备下载: {}", remote_info.path);
                should_request = true;
            } else {
                std::string remote_hash = remote_info.hash;
                std::string local_hash = Hashing::CalculateSHA256(full_path);
                std::string base_hash = m_state_manager->get_base_hash(peer_id, remote_info.path);

                if (local_hash == remote_hash) {
                    g_logger->debug("[Sync] 内容一致，无需更新: {}", remote_info.path);
                    m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                    continue;
                }

                bool local_changed = !base_hash.empty() && (local_hash != base_hash);
                bool remote_changed = !base_hash.empty() && (remote_hash != base_hash);

                if (base_hash.empty() || (!local_changed && remote_changed)) {
                    g_logger->info("[Sync] 正常更新 (本地未修改): {}", remote_info.path);
                    should_request = true;
                } else if (local_changed && !remote_changed) {
                    g_logger->info("[Sync] 本地版本更新，忽略远程旧版本: {}", remote_info.path);
                    m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                    should_request = false;
                } else if (!local_changed && !remote_changed) {
                    should_request = false;
                } else {
                    // 双方都改了 → 冲突处理
                    g_logger->warn("[Sync] ⚠️ 检测到冲突 (双方都修改了): {}", remote_info.path);
                    
                    auto now = std::chrono::system_clock::now();
                    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

                    std::string filename = relative_path.stem().string();
                    std::string ext = relative_path.extension().string();
                    std::string conflict_name = filename + ".conflict." + std::to_string(timestamp) + ext;
                    std::filesystem::path conflict_path = full_path.parent_path() / conflict_name;

                    std::error_code ren_ec;
                    std::filesystem::rename(full_path, conflict_path, ren_ec);

                    if (!ren_ec) {
                        g_logger->warn("[Sync] ⚡ 本地冲突文件已重命名为: {}", conflict_path.filename().string());
                        should_request = true;
                    } else {
                        g_logger->error("[Sync] ❌ 冲突处理失败 (无法重命名): {} | {}", remote_info.path, FormatErrorCode(ren_ec));
                        continue;
                    }
                }
            }

            if (should_request) {
                files_to_request.emplace_back(remote_info, true);
            }
        }
        
        // 3. 批量发送文件请求
        if (!files_to_request.empty()) {
            g_logger->info("[Sync] 批量请求 {} 个文件", files_to_request.size());
            
            boost::asio::post(m_io_context, [this, peer_id, files_to_request]() {
                try {
                    m_with_peer(peer_id, [this, &peer_id, &files_to_request](PeerController* peer_ctrl) {
                        if (!peer_ctrl || !peer_ctrl->is_connected()) return;
                        
                        for (const auto& [remote_info, _] : files_to_request) {
                            nlohmann::json request_msg;
                            request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                            
                            auto resume_info = m_transfer_manager->check_resume_eligibility(
                                remote_info.path, remote_info.hash, remote_info.size);
                            
                            if (resume_info) {
                                request_msg[Protocol::MSG_PAYLOAD] = {
                                    {"path", remote_info.path},
                                    {"start_chunk", resume_info->received_chunks},
                                    {"expected_hash", resume_info->expected_hash},
                                    {"expected_size", resume_info->expected_size}
                                };
                            } else {
                                m_transfer_manager->register_expected_metadata(
                                    remote_info.path, peer_id, remote_info.hash, remote_info.size);
                                request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
                            }
                            
                            m_send_to_peer(request_msg.dump(), peer_ctrl);
                        }
                    });
                } catch (const std::exception& e) {
                    g_logger->error("[Sync] 批量请求文件异常: {}", e.what());
                }
            });
        }
    } catch (const std::exception& e) {
        g_logger->error("[KCP] handle_file_update_batch 异常: {}", e.what());
    }
    });
}

void SyncHandler::handle_file_delete_batch(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    if (!m_state_manager) return;
    
    std::vector<std::string> paths;
    try {
        if (!payload.contains("paths")) {
            g_logger->error("[KCP] file_delete_batch 缺少 paths 字段");
            return;
        }
        for (const auto& path : payload["paths"]) {
            paths.push_back(path.get<std::string>());
        }
    } catch (const std::exception& e) {
        g_logger->error("[KCP] 解析 file_delete_batch 失败: {}", e.what());
        return;
    }
    
    g_logger->info("[KCP] (Destination) 收到批量文件删除: {} 个文件", paths.size());
    
    if (from_peer) {
        from_peer->received_file_count.fetch_add(static_cast<int>(paths.size()));
    }
    
    boost::asio::post(m_worker_pool, [this, paths]() {
        for (const auto& relative_path_str : paths) {
            std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(relative_path_str);
            
            std::error_code ec;
            if (std::filesystem::remove(full_path, ec)) {
                g_logger->debug("[Sync] -> 批量删除: {}", relative_path_str);
                m_state_manager->remove_path_from_map(relative_path_str);
            } else if (ec && ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] ❌ 批量删除失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            }
        }
        g_logger->info("[Sync] 批量删除完成: {} 个文件", paths.size());
    });
}

void SyncHandler::handle_dir_batch(const nlohmann::json& payload, PeerController* from_peer) {
    if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;
    if (!m_state_manager) return;
    
    std::vector<std::string> creates;
    std::vector<std::string> deletes;
    
    try {
        if (payload.contains("creates")) {
            for (const auto& dir : payload["creates"]) {
                creates.push_back(dir.get<std::string>());
            }
        }
        if (payload.contains("deletes")) {
            for (const auto& dir : payload["deletes"]) {
                deletes.push_back(dir.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        g_logger->error("[KCP] 解析 dir_batch 失败: {}", e.what());
        return;
    }
    
    g_logger->info("[KCP] (Destination) 收到批量目录变更: {} 创建, {} 删除", 
                   creates.size(), deletes.size());
    
    if (from_peer) {
        from_peer->received_dir_count.fetch_add(static_cast<int>(creates.size() + deletes.size()));
    }
    
    boost::asio::post(m_worker_pool, [this, creates, deletes]() {
        try {
            // 先处理创建
            for (const auto& dir_path_str : creates) {
                std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                
                std::error_code ec;
                if (std::filesystem::create_directories(full_path, ec) || std::filesystem::exists(full_path)) {
                    m_state_manager->add_dir_to_map(dir_path_str);
                } else if (ec) {
                    g_logger->error("[Sync] ❌ 批量创建目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                }
            }
            
            // 再处理删除（从最深的目录开始）
            std::vector<std::string> sorted_deletes = deletes;
            std::sort(sorted_deletes.begin(), sorted_deletes.end(),
                      [](const std::string& a, const std::string& b) { return a.length() > b.length(); });
            
            for (const auto& dir_path_str : sorted_deletes) {
                std::filesystem::path full_path = m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                
                std::error_code ec;
                std::filesystem::remove_all(full_path, ec);
                
                if (!ec || ec == std::errc::no_such_file_or_directory) {
                    m_state_manager->remove_dir_from_map(dir_path_str);
                } else {
                    g_logger->error("[Sync] ❌ 批量删除目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                }
            }
            
            g_logger->info("[Sync] 批量目录变更完成: {} 创建, {} 删除", creates.size(), deletes.size());
        } catch (const std::exception& e) {
            g_logger->error("[Sync] handle_dir_batch 异常: {}", e.what());
        }
    });
}

}  // namespace VeritasSync
