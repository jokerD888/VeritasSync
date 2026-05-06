#include "VeritasSync/sync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cerrno>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/common/PathUtils.h"
#include "VeritasSync/net/KcpProto.h"
#include "VeritasSync/storage/StateManager.h"

namespace VeritasSync {

// 简单的 RAII 作用域退出辅助类
class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> func) : m_func(std::move(func)) {}
    ~ScopeExit() { if (m_func) m_func(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&& other) noexcept : m_func(std::move(other.m_func)) { other.m_func = nullptr; }
    ScopeExit& operator=(ScopeExit&& other) noexcept {
        if (this != &other) {
            if (m_func) m_func();  // 执行旧的清理操作
            m_func = std::move(other.m_func);
            other.m_func = nullptr;
        }
        return *this;
    }

private:
    std::function<void()> m_func;
};

static constexpr int  DEFAULT_FILE_OPEN_EMFILE_DELAY_MS = 1000;   // 文件句柄耗尽时重试延迟（毫秒）

// --- 上传任务上下文 (用于异步管理状态) ---
struct UploadContext {
    std::string peer_id;
    std::string path;
    std::ifstream file;
    std::string file_hash;
    // 使用 uint32_t 避免超大文件溢出，与 ChunkHeader 一致
    uint32_t total_chunks = 0;
    uint32_t current_chunk = 0;
    int open_retry_count = 0; // 用于异步重试计数

    // 资源复用
    std::vector<char> buffer;
    std::string compressed_data;
    boost::asio::steady_timer timer;

    // 流控阈值：当 KCP 发送队列超过此值时触发流控等待
    static constexpr int CONGESTION_THRESHOLD = 256;

    // 运行时 chunk 大小
    size_t chunk_data_size;

    UploadContext(boost::asio::io_context& ioc, size_t chunk_size) 
        : buffer(chunk_size), timer(ioc), chunk_data_size(chunk_size) {
        compressed_data.reserve(snappy::MaxCompressedLength(chunk_size));
    }
};

// 拒绝 total_chunks 超过 8M 的请求
static constexpr uint32_t MAX_ALLOWED_TOTAL_CHUNKS = 8 * 1024 * 1024;
// 拒绝路径超过 4096 字节的请求
static constexpr size_t MAX_ALLOWED_RELATIVE_PATH_LEN = 4096;

std::optional<TransferManager::UploadRequest> TransferManager::parse_and_validate_upload_request(
    const nlohmann::json& request_payload,
    StateManager* state_manager,
    std::string& error_reason) {

    if (!state_manager) {
        error_reason = "state_manager is null";
        return std::nullopt;
    }

    try {
        if (!request_payload.is_object()) {
            error_reason = "payload is not json object";
            return std::nullopt;
        }

        if (!request_payload.contains("path") || !request_payload["path"].is_string()) {
            error_reason = "missing or invalid path";
            return std::nullopt;
        }

        UploadRequest req;
        req.path = request_payload.at("path").get<std::string>();
        req.start_chunk = request_payload.value("start_chunk", 0u);
        req.expected_hash = request_payload.value("expected_hash", "");
        req.expected_size = request_payload.value("expected_size", 0ull);

        if (req.path.empty()) {
            error_reason = "empty path";
            return std::nullopt;
        }

        if (req.path.size() > MAX_ALLOWED_RELATIVE_PATH_LEN) {
            error_reason = "path too long";
            return std::nullopt;
        }

        if (!PathUtils::is_path_safe(state_manager->get_root_path(), req.path)) {
            error_reason = "path traversal detected";
            return std::nullopt;
        }
        return req;
    } catch (const std::exception& e) {
        error_reason = std::string("json parse failed: ") + e.what();
        return std::nullopt;
    }
}

bool TransferManager::validate_chunk_header(const ChunkHeader& hdr, std::string& error_reason) {
    if (!hdr.valid) {
        error_reason = "invalid chunk header";
        return false;
    }

    if (hdr.file_path.empty()) {
        error_reason = "empty file path";
        return false;
    }

    if (hdr.file_path.size() > MAX_ALLOWED_RELATIVE_PATH_LEN) {
        error_reason = "file path too long";
        return false;
    }

    if (hdr.total_chunks == 0) {
        error_reason = "total_chunks is zero";
        return false;
    }

    if (hdr.total_chunks > MAX_ALLOWED_TOTAL_CHUNKS) {
        error_reason = "total_chunks exceeds limit";
        return false;
    }

    if (hdr.chunk_index >= hdr.total_chunks) {
        error_reason = "chunk_index out of range";
        return false;
    }

    return true;
}

TransferManager::SessionStats TransferManager::get_session_stats() const {

    return {m_session_total.load(), m_session_done.load()};
}
TransferManager::TransferManager(StateManager* sm, boost::asio::io_context& io_context,
                                 boost::asio::thread_pool& pool, SendCallback send_cb,
                                 size_t chunk_size, TransferConfig config)
    : CHUNK_DATA_SIZE(chunk_size), m_state_manager(sm), m_io_context(io_context),
      m_worker_pool(pool), m_send_callback(std::move(send_cb)), m_transfer_config(std::move(config)) {
    // 启动时从 DB 恢复未完成的下载任务
    load_download_state();
}

void TransferManager::queue_upload(const std::string& peer_id, const nlohmann::json& request_payload) {
    if (peer_id.empty()) return;

    std::string validation_error;
    auto request = parse_and_validate_upload_request(request_payload, m_state_manager, validation_error);
    if (!request) {
        g_logger->warn("[Transfer] 拒绝无效上传请求: peer={}, reason={}", peer_id, validation_error);
        return;
    }

    if (request->start_chunk > 0) {
        g_logger->info("[Transfer] 收到续传请求: {} -> {} (start_chunk={})",
                      request->path, peer_id, request->start_chunk);
    } else {
        g_logger->info("[Transfer] 开始处理文件请求: {} -> {}", request->path, peer_id);
    }

    // 1. UI 占位
    {
        std::lock_guard<std::mutex> lock(m_transfer_mutex);
        m_sending_files[make_sending_key(peer_id, request->path)] = {0, 0};
        m_session_total++;
    }

    // 创建上下文，持有 io_context 用于定时器
    auto ctx = std::make_shared<UploadContext>(m_io_context, CHUNK_DATA_SIZE);
    ctx->peer_id = peer_id;
    ctx->path = request->path;

    const uint32_t start_chunk = request->start_chunk;
    const std::string expected_hash = request->expected_hash;
    const uint64_t expected_size = request->expected_size;


    // 2. 启动异步任务链
    boost::asio::post(m_worker_pool, [self = shared_from_this(), ctx, start_chunk, expected_hash, expected_size]() {
        // --- 准备阶段 ---
        if (!self->m_state_manager) return;

        ctx->file_hash = self->m_state_manager->get_file_hash(ctx->path);
        std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(ctx->path);
        full_path.make_preferred(); // 确保 Windows 下使用反斜杠

#ifdef _WIN32
        // Windows 长路径增强
        std::wstring wpath = full_path.wstring();
        if (full_path.is_absolute() && wpath.substr(0, 4) != L"\\\\?\\") {
            wpath = L"\\\\?\\" + wpath;
            full_path = std::filesystem::path(wpath);
        }
#endif

        // 先检查文件物理是否存在
        std::error_code fs_ec;
        if (!std::filesystem::exists(full_path, fs_ec)) {
            g_logger->error("[Transfer] 文件物理不存在: {} (OS Error: {})", PathToUtf8(full_path), fs_ec.message());
            {
                std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                self->m_sending_files.erase(make_sending_key(ctx->peer_id, ctx->path));
            }
            self->m_session_done++;
            return;
        }
        
        // 【断点续传】校验并设置起始位置
        uint32_t actual_start_chunk = 0;
        if (start_chunk > 0) {
            // 获取当前文件信息进行校验
            std::string current_hash = ctx->file_hash;
            uint64_t current_size = std::filesystem::file_size(full_path, fs_ec);
            
            bool hash_match = expected_hash.empty() || (current_hash == expected_hash);
            bool size_match = (expected_size == 0) || (current_size == expected_size);
            
            if (!hash_match) {
                g_logger->warn("[Transfer] 续传校验失败: hash 不匹配，从头开始发送 {} (本地: {}..., 期望: {}...)", 
                              ctx->path, current_hash.substr(0, 8), expected_hash.substr(0, 8));
            } else if (!size_match) {
                g_logger->warn("[Transfer] 续传校验失败: size 不匹配，从头开始发送 {} (本地: {}, 期望: {})", 
                              ctx->path, current_size, expected_size);
            } else {
                actual_start_chunk = start_chunk;
                g_logger->info("[Transfer] 续传校验通过: {} 从 chunk #{} 开始", ctx->path, actual_start_chunk);
            }
        }
        
        // 设置起始块
        ctx->current_chunk = static_cast<uint32_t>(actual_start_chunk);

        // --- 直接进入循环，由循环内部负责按需打开和关闭句柄 ---


        // --- 定义递归上传逻辑 ---
        auto upload_loop = [self, ctx, full_path](auto& loop_ref) -> void {
            // 如果句柄由于刚开始或由于流控被关闭，在此处异步重开
            if (!ctx->file.is_open()) {
                ctx->file.clear(); // 关键：清除之前的错误状态位
                ctx->file.open(full_path, std::ios::binary | std::ios::in);
                
                if (!ctx->file.is_open()) {
                    std::string sys_err = GetLastSystemError();
                    // 处理句柄耗尽 (EMFILE/ENFILE) 与 锁定 等待
                    int wait_ms = (errno == EMFILE || errno == ENFILE) ? DEFAULT_FILE_OPEN_EMFILE_DELAY_MS : self->m_transfer_config.file_open_retry_delay_ms;

                    if (ctx->open_retry_count < self->m_transfer_config.file_open_max_retries) {
                        ctx->open_retry_count++;
                        g_logger->warn("[Transfer] ⚠️ 文件打开失败(第{}次), 退让{}ms后重试: {} | {}", 
                                      ctx->open_retry_count, wait_ms, ctx->path, sys_err);
                        
                        // 【异步退让核心】不阻塞当前线程，设置闹钟后立即返回
                        ctx->timer.expires_after(std::chrono::milliseconds(wait_ms));
                        ctx->timer.async_wait([self, ctx, loop_ref](const boost::system::error_code& ec) {
                            if (!ec) {
                                // 时间到了，把自己 post 会线程池继续干活
                                boost::asio::post(self->m_worker_pool, [loop_ref]() {
                                    loop_ref(loop_ref);
                                });
                            }
                        });
                        return; // 释放线程池资源
                    } else {
                        g_logger->error("[Transfer] ❌ 无法打开文件(重试{}次后放弃): {} | {}", self->m_transfer_config.file_open_max_retries, ctx->path, sys_err);
                        {
                            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                            self->m_sending_files.erase(make_sending_key(ctx->peer_id, ctx->path));
                        }
                        self->m_session_done++;
                        return;
                    }
                }

                // --- 打开成功，执行初次/恢复初始化 ---
                ctx->open_retry_count = 0; // 重置计数器

                // 首次打开时计算总块数
                if (ctx->total_chunks == 0) {
                    ctx->file.seekg(0, std::ios::end);
                    std::streamsize size = ctx->file.tellg();
                    ctx->total_chunks = (size > 0) ? static_cast<uint32_t>((size + ctx->chunk_data_size - 1) / ctx->chunk_data_size) : 1;
                    
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    auto it = self->m_sending_files.find(make_sending_key(ctx->peer_id, ctx->path));
                    if (it != self->m_sending_files.end()) {
                        it->second.total_chunks = static_cast<uint32_t>(ctx->total_chunks);
                    }
                }
                
                // 定位并继续
                ctx->file.seekg(static_cast<std::streampos>(ctx->current_chunk) * ctx->chunk_data_size, std::ios::beg);
            }
            // 使用 while 循环来处理非阻塞的情况，提高效率
            // 将"读→压缩→组包→发送"提取为局部 lambda，缩小循环体
            auto send_one_chunk = [&]() -> int {
                // 读取数据
                ctx->file.read(ctx->buffer.data(), ctx->chunk_data_size);
                std::streamsize bytes_read = ctx->file.gcount();

                // 压缩
                snappy::Compress(ctx->buffer.data(), bytes_read, &ctx->compressed_data);

                // 组包：MsgType(1) + PathLen(2) + Path + ChunkIdx(4) + TotalChunks(4) + CompressedData
                std::string binary_packet;
                static constexpr size_t CHUNK_HEADER_OVERHEAD = 1 + 2 + 4 + 4;  // MsgType + PathLen + ChunkIdx + TotalChunks
                binary_packet.reserve(CHUNK_HEADER_OVERHEAD + ctx->path.length() + ctx->compressed_data.length());
                binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
                append_uint16(binary_packet, static_cast<uint16_t>(ctx->path.length()));
                binary_packet.append(ctx->path);
                append_uint32(binary_packet, ctx->current_chunk);
                append_uint32(binary_packet, ctx->total_chunks);
                binary_packet.append(ctx->compressed_data);

                // 加密已下沉到 PeerController (KCP 层)，直接发送明文
                return self->m_send_callback(ctx->peer_id, binary_packet);
            };

            while (ctx->current_chunk < ctx->total_chunks) {
                int pending = send_one_chunk();

                // 【断点续传】检测连接是否已断开
                if (pending < 0) {
                    g_logger->warn("[Transfer] 连接已断开，终止发送: {} (已发送 {}/{} 块)",
                                  ctx->path, ctx->current_chunk, ctx->total_chunks);
                    ctx->file.close();
                    {
                        std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                        self->m_sending_files.erase(make_sending_key(ctx->peer_id, ctx->path));
                    }
                    self->m_session_done++;
                    return;
                }

                // 更新进度
                ctx->current_chunk++;
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    auto it = self->m_sending_files.find(make_sending_key(ctx->peer_id, ctx->path));
                    if (it != self->m_sending_files.end()) {
                        it->second.sent_chunks = ctx->current_chunk;
                        it->second.last_active = std::chrono::steady_clock::now();
                    }
                }

                // --- 智能流控 ---
                if (pending > ctx->CONGESTION_THRESHOLD) {
                    // 在挂起等待前，主动关闭文件句柄！
                    // 这确保了只有当前正在活跃读写的线程（通常只有几个）会占用句柄。
                    ctx->file.close();

                    // 增加等待时间，给 KCP 更多时间消耗发送队列
                    int sleep_ms = (pending > ctx->CONGESTION_THRESHOLD * self->m_transfer_config.congestion_high_multiplier) ? self->m_transfer_config.congestion_wait_high_ms : self->m_transfer_config.congestion_wait_low_ms;
                    ctx->timer.expires_after(std::chrono::milliseconds(sleep_ms));
                    ctx->timer.async_wait([self, ctx, loop_ref](const boost::system::error_code& ec) {
                        if (!ec) {
                            boost::asio::post(self->m_worker_pool, [loop_ref]() {
                                loop_ref(loop_ref);
                            });
                        }
                    });
                    return;
                }
                // 如果没有积压，continue while 循环，直接发下一块，效率最高
            }

            // --- 循环结束：清理与收尾 ---
            if (ctx->current_chunk >= ctx->total_chunks) {
                ctx->file.close(); // 传输完成，关闭句柄
                // StateManager 可能在 stop() 中被置空
                if (!ctx->file_hash.empty() && self->m_state_manager) {
                    self->m_state_manager->record_file_sent(ctx->peer_id, ctx->path, ctx->file_hash);
                }
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    self->m_sending_files.erase(make_sending_key(ctx->peer_id, ctx->path));
                }
                self->m_session_done++;
                g_logger->info("[Transfer] 文件发送完成: {} (共 {} 块)", ctx->path, ctx->total_chunks);
            }
        };

        // 启动循环
        upload_loop(upload_loop);
    });
}
// ═══════════════════════════════════════════════════════════════
// handle_chunk 的子步骤
// ═══════════════════════════════════════════════════════════════

TransferManager::ChunkHeader TransferManager::parse_chunk_payload(const std::string& payload) {
    ChunkHeader hdr;
    const char* data_ptr = payload.c_str();
    size_t data_len = payload.length();

    uint16_t path_len = read_uint16(data_ptr, data_len);
    if (path_len == 0 || data_len < path_len) return hdr;

    hdr.file_path.assign(data_ptr, path_len);
    data_ptr += path_len;
    data_len -= path_len;

    // 至少要包含 chunk_index(4) + total_chunks(4)
    if (data_len < sizeof(uint32_t) * 2) return hdr;

    hdr.chunk_index = read_uint32(data_ptr, data_len);
    hdr.total_chunks = read_uint32(data_ptr, data_len);


    // Snappy 解压 — CPU 密集操作
    if (data_len > 0) {
        if (!snappy::Uncompress(data_ptr, data_len, &hdr.uncompressed_data)) {
            g_logger->error("[Transfer] Snappy 解压失败: {}", hdr.file_path);
            return hdr;
        }
    }

    hdr.valid = true;
    return hdr;
}

TransferManager::ChunkLookupResult TransferManager::lookup_or_create_receiving(
    const std::string& file_path_str,
    uint32_t total_chunks,
    const std::string& peer_id) {

    ChunkLookupResult result;

    // StateManager 可能在 stop() 中被置空
    if (!m_state_manager) {
        g_logger->warn("[Transfer] StateManager 已释放，跳过接收: {}", file_path_str);
        return result;
    }

    // 路径遍历攻击防护：验证网络传入的路径不会逃逸出同步根目录
    if (!PathUtils::is_path_safe(m_state_manager->get_root_path(), file_path_str)) {
        g_logger->error("[Transfer] ⚠️ 路径安全检查失败，拒绝接收: {}", file_path_str);
        return result;  // result.recv_ptr 为 nullptr，上层会跳过
    }

    std::lock_guard<std::mutex> map_lock(m_transfer_mutex);

    auto it = m_receiving_files.find(file_path_str);
    bool is_new_task = (it == m_receiving_files.end());
    result.need_open_stream = is_new_task;

    // 统一构造路径，避免 if-else 分支重复
    std::filesystem::path relative_path = Utf8ToPath(file_path_str);
    result.full_path = m_state_manager->get_root_path() / relative_path;
    result.temp_path = result.full_path;
    result.temp_path += ".veritas_tmp";

    // 两个分支对应两种接收场景：
    //
    // [新任务] first chunk 直接到达，m_receiving_files 无此文件记录。
    //   → 创建完整的 ReceivingFile（bitmap/temp_path/total_chunks 全设置）
    //
    // [预注册] chunk 到达前，register_expected_metadata 已创建占位记录。
    //   → else 分支补全占位记录的 temp_path/bitmap 等字段。
    //     预注册的场景：全新下载、续传不适用（零进度/临时文件丢失/源文件已变）。
    //     反之，断点续传（check_resume_eligibility 返回有效）不走预注册。
    if (is_new_task) {
        auto new_file = std::make_shared<ReceivingFile>();

        new_file->temp_path = PathToUtf8(result.temp_path);
        new_file->total_chunks = total_chunks;
        // 初始化 bitmap 用于去重
        new_file->received_bitmap.resize(total_chunks, false);
        new_file->peer_id = peer_id;
        new_file->busy = true;
        result.need_create_dirs = result.full_path.has_parent_path();

        m_receiving_files[file_path_str] = new_file;
        result.recv_ptr = new_file;
        m_session_total++;
        g_logger->info("[Transfer] 开始接收: {} ({} 块)", file_path_str, total_chunks);
    } else {
        result.recv_ptr = it->second;
        result.recv_ptr->peer_id = peer_id;
        if (result.recv_ptr->total_chunks == 0) {
            result.recv_ptr->total_chunks = total_chunks;
            // 同步初始化 bitmap，避免延迟 resize 的竞态窗口
            result.recv_ptr->received_bitmap.resize(total_chunks, false);
        } else if (result.recv_ptr->total_chunks != total_chunks) {
            // total_chunks 不一致说明源文件变化或协议异常，重置接收状态
            g_logger->warn("[Transfer] ⚠️ total_chunks 不一致 (记录={}, 新={})，重置接收: {}",
                          result.recv_ptr->total_chunks, total_chunks, file_path_str);
            if (result.recv_ptr->file_stream.is_open()) {
                result.recv_ptr->file_stream.close();
            }
            if (!result.recv_ptr->temp_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(Utf8ToPath(result.recv_ptr->temp_path), ec);
            }
            result.recv_ptr->total_chunks = total_chunks;
            result.recv_ptr->received_chunks = 0;
            result.recv_ptr->received_bitmap.assign(total_chunks, false);
            result.recv_ptr->temp_path = PathToUtf8(result.temp_path);
            result.need_open_stream = true;
            result.need_create_dirs = result.full_path.has_parent_path();
        }

        if (result.recv_ptr->temp_path.empty()) {
            result.recv_ptr->temp_path = PathToUtf8(result.temp_path);
            result.need_open_stream = true;
            result.need_create_dirs = result.full_path.has_parent_path();
            m_session_total++;
            g_logger->info("[Transfer] 开始接收（从预注册）: {} ({} 块)", file_path_str, total_chunks);
        }
        result.recv_ptr->busy = true;
    }

    return result;
}

void TransferManager::finalize_received_file(
    const std::string& file_path_str,
    std::shared_ptr<ReceivingFile>& recv_ptr,
    const std::string& peer_id) {

    ScopeExit finalize_guard([this, file_path_str]() {
        m_session_done++;
        std::lock_guard<std::mutex> map_lock(m_transfer_mutex);
        m_receiving_files.erase(file_path_str);
        // 同时清理 DB 中的持久化记录
        if (m_state_manager && m_state_manager->get_db().is_valid()) {
            m_state_manager->get_db().remove_download_task(file_path_str);
        }
    });

    recv_ptr->file_stream.close();

    // StateManager 可能在 stop() 中被置空
    if (!m_state_manager) {
        g_logger->warn("[Transfer] StateManager 已释放，跳过文件完成处理: {}", file_path_str);
        return;
    }

    std::filesystem::path relative_path = Utf8ToPath(file_path_str);
    std::filesystem::path final_path = m_state_manager->get_root_path() / relative_path;
    std::filesystem::path temp_path_obj = Utf8ToPath(recv_ptr->temp_path);

    std::error_code ec;
    std::filesystem::rename(temp_path_obj, final_path, ec);

    if (!ec) {
        // 计算本地哈希并与对端声称的 expected_hash 校验
        // 原因：KCP 虽然有序传输，但不保证数据完整性；UDP 校验和不可靠。
        // 必须本地校验哈希，否则损坏的数据会污染同步状态。
        if (!peer_id.empty() && m_state_manager) {
            std::string new_hash = Hashing::CalculateSHA256(final_path);

            if (recv_ptr->expected_hash.empty()) {
                // 未经预注册流程就收到了完整文件——说明对端绕过了握手协议
                g_logger->error("[Transfer] ❌ 文件完成但无预期哈希（未经预注册），无法校验完整性: {}", file_path_str);
                m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
                m_state_manager->mark_file_received(file_path_str, new_hash);
            } else if (new_hash == recv_ptr->expected_hash) {
                g_logger->info("[Transfer] ✅ 下载完成: {} (hash 校验通过)", file_path_str);
                m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
                m_state_manager->mark_file_received(file_path_str, new_hash);
            } else {
                g_logger->error("[Transfer] ❌ 文件损坏: {} (本地 {}..., 期望 {}...)，已删除",
                                file_path_str, new_hash.substr(0, 8),
                                recv_ptr->expected_hash.substr(0, 8));
                std::error_code rm_ec;
                std::filesystem::remove(final_path, rm_ec);
                // 不记录 sync_success，文件不在同步状态中
            }
        } else {
            g_logger->info("[Transfer] ✅ 下载完成: {} (无校验)", file_path_str);
        }
    } else {
        g_logger->error("[Transfer] 重命名失败: {} -> {} | {}", recv_ptr->temp_path, PathToUtf8(final_path), ec.message());
    }
}


// ═══════════════════════════════════════════════════════════════

void TransferManager::handle_chunk(std::string payload, const std::string& peer_id) {
    // payload 按值传入，直接 move 进 lambda，避免热路径上 ~16KB 的额外拷贝
    boost::asio::post(m_worker_pool, [self = shared_from_this(), payload = std::move(payload), peer_id]() {
        std::shared_ptr<ReceivingFile> recv_ptr;  // 提到 try 外部，供 catch 块清理用
        try {
            // 阶段 0: 解析头部 + Snappy 解压（无锁）
            auto hdr = parse_chunk_payload(payload);
            std::string chunk_error;
            if (!validate_chunk_header(hdr, chunk_error)) {
                g_logger->warn("[Transfer] 丢弃无效chunk: peer={}, path='{}', reason={}",
                              peer_id, hdr.file_path, chunk_error);
                return;
            }

            // 阶段 A: 短暂全局锁，查找/创建 ReceivingFile
            auto lookup = self->lookup_or_create_receiving(hdr.file_path, hdr.total_chunks, peer_id);
            recv_ptr = lookup.recv_ptr;
            if (!recv_ptr) {
                g_logger->warn("[Transfer] 接收上下文创建失败，跳过chunk: peer={}, path={}",
                              peer_id, hdr.file_path);
                return;
            }

            // 阶段 B: per-file 锁，执行所有 I/O 操作
            std::lock_guard<std::mutex> file_lock(recv_ptr->file_mutex);


            auto now = std::chrono::steady_clock::now();
            recv_ptr->last_active = now;

            // 创建目录
            if (lookup.need_create_dirs) {
                std::error_code dir_ec;
                std::filesystem::create_directories(lookup.full_path.parent_path(), dir_ec);
                if (dir_ec) {
                    g_logger->error("[Transfer] ❌ 无法创建目录: {} | {}", PathToUtf8(lookup.full_path.parent_path()), FormatErrorCode(dir_ec));
                    self->m_session_done++;
                    std::lock_guard<std::mutex> map_lock(self->m_transfer_mutex);
                    self->m_receiving_files.erase(hdr.file_path);
                    return;
                }
            }

            // 打开文件流
            if (!recv_ptr->file_stream.is_open()) {
                std::filesystem::path temp_path = Utf8ToPath(recv_ptr->temp_path);

                if (lookup.need_open_stream && !std::filesystem::exists(temp_path)) {
                    recv_ptr->file_stream.open(temp_path, std::ios::binary | std::ios::out);
                } else {
                    recv_ptr->file_stream.open(temp_path, std::ios::binary | std::ios::in | std::ios::out);
                }

                if (!recv_ptr->file_stream.is_open()) {
                    std::string sys_err = GetLastSystemError();
                    g_logger->error("[Transfer] ❌ 无法打开临时文件: {} | {}", recv_ptr->temp_path, sys_err);
                    std::error_code cleanup_ec;
                    std::filesystem::remove(Utf8ToPath(recv_ptr->temp_path), cleanup_ec);
                    self->m_session_done++;
                    std::lock_guard<std::mutex> map_lock(self->m_transfer_mutex);
                    self->m_receiving_files.erase(hdr.file_path);
                    return;
                }
                g_logger->debug("[Transfer] 打开文件流: {}", recv_ptr->temp_path);
            }

            // 使用 bitmap 去重：如果这个 chunk 已经收过，跳过写入
            // 确保 bitmap 大小足够
            if (recv_ptr->received_bitmap.size() <= hdr.chunk_index) {
                recv_ptr->received_bitmap.resize(hdr.total_chunks, false);
            }
            if (recv_ptr->received_bitmap[hdr.chunk_index]) {
                // 重复 chunk（KCP 重传），跳过写入但更新活跃时间
                g_logger->debug("[Transfer] 跳过重复 chunk {}/{}: {}", 
                               hdr.chunk_index, hdr.total_chunks, hdr.file_path);
                recv_ptr->busy = false;
                return;
            }

            // 写入数据
            size_t offset = static_cast<size_t>(hdr.chunk_index) * self->CHUNK_DATA_SIZE;
            recv_ptr->file_stream.seekp(offset);
            recv_ptr->file_stream.write(hdr.uncompressed_data.data(), hdr.uncompressed_data.size());

            if (recv_ptr->file_stream.fail()) {
                std::string sys_err = GetLastSystemError();
                g_logger->error("[Transfer] ❌ 文件写入失败 (chunk {}): {} | {}", hdr.chunk_index, hdr.file_path, sys_err);
                recv_ptr->file_stream.close();
                std::error_code cleanup_ec;
                std::filesystem::remove(Utf8ToPath(recv_ptr->temp_path), cleanup_ec);
                if (cleanup_ec) {
                    g_logger->warn("[Transfer] 清理临时文件失败: {} | {}", recv_ptr->temp_path, cleanup_ec.message());
                }
                // 写失败也要递增 session_done，保持计数器一致
                self->m_session_done++;
                std::lock_guard<std::mutex> map_lock(self->m_transfer_mutex);
                self->m_receiving_files.erase(hdr.file_path);
                return;
            }
            // 标记 bitmap 并递增计数器（仅新 chunk 时）
            recv_ptr->received_bitmap[hdr.chunk_index] = true;
            recv_ptr->received_chunks++;

            // 阶段 C: 检查完成 → 调用收尾方法
            if (recv_ptr->received_chunks >= recv_ptr->total_chunks) {
                self->finalize_received_file(hdr.file_path, recv_ptr, peer_id);
                recv_ptr->busy = false;  // 完成路径统一复位，与 else 分支对称
            } else {
                recv_ptr->busy = false;
            }
        } catch (const std::exception& e) {
            g_logger->error("[Transfer] handle_chunk 异常: {}", e.what());
            // 异常路径下必须重置 busy 标志并关闭文件流，
            // 否则该条目将永远为 busy，cleanup_stale_buffers 和
            // cancel_receives_for_peer 都会跳过它，导致永久僵尸和句柄泄漏
            if (recv_ptr) {
                if (recv_ptr->file_stream.is_open()) {
                    recv_ptr->file_stream.close();
                }
                recv_ptr->busy = false;
            }
        } catch (...) {
            g_logger->error("[Transfer] handle_chunk 未知异常");
            if (recv_ptr) {
                if (recv_ptr->file_stream.is_open()) {
                    recv_ptr->file_stream.close();
                }
                recv_ptr->busy = false;
            }
        }
    });
}

std::vector<TransferStatus> TransferManager::get_active_transfers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::vector<TransferStatus> list;
    auto now = std::chrono::steady_clock::now();

    // 速度计算辅助 lambda，消除上传/下载的逻辑重复
    auto update_speed = [&](auto current_chunks, auto& last_tick_chunks,
                            auto& last_tick_time, auto& current_speed) {
        std::chrono::duration<double> elapsed = now - last_tick_time;
        if (elapsed.count() >= m_transfer_config.speed_update_interval_sec) {
            current_speed = ((current_chunks - last_tick_chunks) * CHUNK_DATA_SIZE) / elapsed.count();
            last_tick_chunks = current_chunks;
            last_tick_time = now;
        }
    };

    // 1. 处理下载任务 (Receiving)
    for (auto& [path, recv_ptr] : m_receiving_files) {
        auto& recv = *recv_ptr;
        update_speed(recv.received_chunks.load(), recv.last_tick_chunks,
                     recv.last_tick_time, recv.current_speed);

        auto time_since_active = std::chrono::duration_cast<std::chrono::milliseconds>(now - recv.last_active).count();
        bool stalled = (time_since_active > m_transfer_config.stall_threshold_ms);
        double display_speed = stalled ? 0.0 : recv.current_speed;
        float prog = (recv.total_chunks > 0) ? (static_cast<float>(recv.received_chunks.load()) / recv.total_chunks * 100.0f) : 0.0f;

        list.push_back({path, recv.total_chunks, recv.received_chunks.load(), prog,
                        false, display_speed, stalled});
    }

    // 2. 处理上传任务 (Sending)
    for (auto& [path, send] : m_sending_files) {
        update_speed(send.sent_chunks, send.last_tick_chunks,
                     send.last_tick_time, send.current_speed);

        auto time_since_active = std::chrono::duration_cast<std::chrono::milliseconds>(now - send.last_active).count();
        bool stalled = (time_since_active > m_transfer_config.stall_threshold_ms);
        double display_speed = stalled ? 0.0 : send.current_speed;
        float prog = (send.total_chunks > 0) ? (static_cast<float>(send.sent_chunks) / send.total_chunks * 100.0f) : 0.0f;

        list.push_back({path, send.total_chunks, send.sent_chunks, prog,
                        true, display_speed, stalled});
    }

    return list;
}

void TransferManager::cleanup_stale_buffers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = m_receiving_files.begin(); it != m_receiving_files.end(); ++it) {
        auto& recv = *it->second;
        // 如果条目正在被 handle_chunk 处理，跳过
        if (recv.busy) continue;
        
        // 接收超时：只关闭文件流，保留记录和临时文件以便续传
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - recv.last_active);
        if (duration.count() > m_transfer_config.receive_timeout_minutes) {
            if (recv.file_stream.is_open()) {
                g_logger->warn("[Transfer] ⏰ 接收超时，关闭文件流（保留状态等待续传）: {}", it->first);
                recv.file_stream.close();
            }
            // 【断点续传】不删除记录和临时文件，由 cancel_receives_for_peer 或后续校验决定
        }
    }

    // 周期性持久化：将当前下载任务状态写入 DB
    persist_download_state();
}

// ═══════════════════════════════════════════════════════════════
// 断点续传相关方法
// ═══════════════════════════════════════════════════════════════

void TransferManager::cancel_receives_for_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    
    int cleaned_count = 0;
    for (auto it = m_receiving_files.begin(); it != m_receiving_files.end(); ) {
        auto& recv = *it->second;
        if (recv.peer_id == peer_id) {
            // 如果 handle_chunk 正在处理这个条目，跳过
            if (recv.busy) {
                g_logger->info("[Transfer] 任务 {} 正在处理中，跳过清理", it->first);
                ++it;
                continue;
            }
            
            // 关闭文件流，释放系统句柄
            if (recv.file_stream.is_open()) {
                recv.file_stream.close();
            }
            
            g_logger->info("[Transfer] 清理来自 {} 的任务: {} ({}/{} chunks)，保留记录等待续传",
                          peer_id, it->first,
                          recv.received_chunks.load(), recv.total_chunks);
            
            // 解除 peer 关联，标记为"无主"状态
            // 保留临时文件和 DB 记录，便于后续续传
            recv.peer_id.clear();
            
            ++it;
            cleaned_count++;
        } else {
            ++it;
        }
    }
    
    if (cleaned_count > 0) {
        g_logger->info("[Transfer] 已清理 {} 的 {} 个任务（保留待续传）", peer_id, cleaned_count);
    }
}

void TransferManager::cancel_sends_for_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::string prefix = peer_id + '\0';
    int cancelled_count = 0;
    for (auto it = m_sending_files.begin(); it != m_sending_files.end(); ) {
        if (it->first.substr(0, prefix.size()) == prefix) {
            g_logger->info("[Transfer] 清理上传任务: {} (key={})", peer_id, it->first);
            it = m_sending_files.erase(it);
            cancelled_count++;
        } else {
            ++it;
        }
    }
    if (cancelled_count > 0) {
        g_logger->info("[Transfer] 已清理 {} 的 {} 个上传任务", peer_id, cancelled_count);
    }
}

std::vector<std::string> TransferManager::get_pending_receives_for_peer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::vector<std::string> result;
    for (const auto& [path, recv_ptr] : m_receiving_files) {
        if (recv_ptr->peer_id == peer_id &&
            recv_ptr->received_chunks < recv_ptr->total_chunks) {
            result.push_back(path);
        }
    }
    return result;
}

std::optional<TransferManager::ResumeInfo> TransferManager::check_resume_eligibility(
    const std::string& path,
    const std::string& remote_hash,
    uint64_t remote_size) {
    
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    
    auto it = m_receiving_files.find(path);
    if (it == m_receiving_files.end()) {
        return std::nullopt;  // 没有未完成任务
    }
    
    auto& rf = *it->second;
    
    // 检查是否已完成
    if (rf.total_chunks > 0 && rf.received_chunks >= rf.total_chunks) {
        g_logger->debug("[Transfer] 文件已完成，无需续传: {}", path);
        return std::nullopt;
    }
    
    // 检查是否有有效进度
    if (rf.received_chunks == 0) {
        g_logger->debug("[Transfer] 尚未收到任何块，无需续传: {}", path);
        return std::nullopt;
    }
    
    // 检查临时文件是否存在
    if (!std::filesystem::exists(rf.temp_path)) {
        g_logger->warn("[Transfer] 临时文件已不存在，无法续传: {}", path);
        m_receiving_files.erase(it);
        return std::nullopt;
    }
    
    // 校验源文件是否变化（hash）
    if (!rf.expected_hash.empty() && !remote_hash.empty() && rf.expected_hash != remote_hash) {
        g_logger->warn("[Transfer] 源文件 hash 已变化，需要重新传输: {} (本地记录: {}..., 远程: {}...)", 
                      path, rf.expected_hash.substr(0, 8), remote_hash.substr(0, 8));
        // 删除旧的临时文件和记录
        if (rf.file_stream.is_open()) rf.file_stream.close();
        std::error_code ec;
        std::filesystem::remove(rf.temp_path, ec);
        m_receiving_files.erase(it);
        return std::nullopt;
    }
    
    // 校验源文件是否变化（size）
    if (rf.expected_size > 0 && remote_size > 0 && rf.expected_size != remote_size) {
        g_logger->warn("[Transfer] 源文件大小已变化，需要重新传输: {} (本地记录: {}, 远程: {})", 
                      path, rf.expected_size, remote_size);
        if (rf.file_stream.is_open()) rf.file_stream.close();
        std::error_code ec;
        std::filesystem::remove(rf.temp_path, ec);
        m_receiving_files.erase(it);
        return std::nullopt;
    }
    
    // 可以续传
    // 计算从 0 开始连续已收到的 chunk 数（而非总计数），
    // 避免乱序到达时 start_chunk 跳过空洞导致文件损坏
    uint32_t contiguous = 0;
    for (size_t i = 0; i < rf.received_bitmap.size(); ++i) {
        if (!rf.received_bitmap[i]) break;
        contiguous++;
    }

    g_logger->info("[Transfer] ✅ 可以续传: {} ({}/{} chunks contiguous, {}/{} total, {:.1f}%)",
                  path, contiguous, rf.total_chunks,
                  rf.received_chunks.load(), rf.total_chunks,
                  rf.total_chunks > 0 ? (100.0 * contiguous / rf.total_chunks) : 0.0);

    return ResumeInfo{
        path,
        contiguous,
        rf.total_chunks,
        rf.expected_hash,
        rf.expected_size,
        rf.temp_path
    };
}

void TransferManager::register_expected_metadata(
    const std::string& path,
    const std::string& peer_id,
    const std::string& hash,
    uint64_t size) {
    
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    
    auto it = m_receiving_files.find(path);
    if (it != m_receiving_files.end()) {
        auto& rf = *it->second;
        // 如果文件已有活跃进度，拒绝覆盖元数据，防止校验信息被偷换
        if (rf.received_chunks > 0) {
            g_logger->warn("[Transfer] 拒绝覆盖活跃任务的元数据: {} (已收 {} chunks, 现有hash={}...)",
                          path, rf.received_chunks.load(),
                          rf.expected_hash.empty() ? "空" : rf.expected_hash.substr(0, 8));
            return;
        }
        rf.peer_id = peer_id;
        rf.expected_hash = hash;
        rf.expected_size = size;
        g_logger->debug("[Transfer] 更新接收任务元数据: {} (hash={}..., size={})",
                       path, hash.substr(0, 8), size);
    } else {
        // 创建新的占位记录（actual file_stream 和 temp_path 在 handle_chunk 时填充）
        auto rf = std::make_shared<ReceivingFile>();
        rf->peer_id = peer_id;
        rf->expected_hash = hash;
        rf->expected_size = size;
        rf->last_active = std::chrono::steady_clock::now();
        m_receiving_files[path] = std::move(rf);
        g_logger->debug("[Transfer] 预注册接收任务: {} (hash={}..., size={})", 
                       path, hash.substr(0, 8), size);
    }
}

// ═══════════════════════════════════════════════════════════════
// 断点续传持久化
// ═══════════════════════════════════════════════════════════════

// vector<bool> → 字节数组（每 8 个 bit 打包为 1 个 byte）
static std::vector<uint8_t> serialize_bitmap(const std::vector<bool>& bitmap) {
    if (bitmap.empty()) return {};
    size_t bytes = (bitmap.size() + 7) / 8;
    std::vector<uint8_t> data(bytes, 0);
    for (size_t i = 0; i < bitmap.size(); ++i) {
        if (bitmap[i]) data[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    }
    return data;
}

// 字节数组 → vector<bool>
static std::vector<bool> deserialize_bitmap(const std::vector<uint8_t>& data, size_t count) {
    std::vector<bool> bitmap(count, false);
    for (size_t i = 0; i < count; ++i) {
        if (data[i / 8] & static_cast<uint8_t>(1 << (i % 8))) bitmap[i] = true;
    }
    return bitmap;
}

void TransferManager::persist_download_state() {
    if (!m_state_manager) return;
    auto& db = m_state_manager->get_db();
    if (!db.is_valid()) return;

    // 注意：调用者必须已持有 m_transfer_mutex（如 cleanup_stale_buffers），
    // 否则需在调用处自行加锁
    if (m_receiving_files.empty()) return;

    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 事务批量写入，减少 SQLite I/O
    Database::TransactionGuard txn(db);
    for (const auto& [path, recv_ptr] : m_receiving_files) {
        auto& recv = *recv_ptr;
        DownloadTaskRecord task;
        task.path = path;
        task.peer_id = recv.peer_id;
        task.total_chunks = recv.total_chunks;
        task.bitmap_data = serialize_bitmap(recv.received_bitmap);
        task.expected_hash = recv.expected_hash;
        task.expected_size = recv.expected_size;
        task.temp_path = recv.temp_path;
        task.last_active = now_ts;
        db.save_download_task(task);
    }
    txn.commit();
}

void TransferManager::load_download_state() {
    if (!m_state_manager) return;
    auto& db = m_state_manager->get_db();
    if (!db.is_valid()) return;

    auto tasks = db.load_all_download_tasks();
    if (tasks.empty()) return;

    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    int restored = 0;
    for (auto& task : tasks) {
        // 校验临时文件是否还在
        if (!std::filesystem::exists(task.temp_path)) {
            g_logger->warn("[Transfer] 断点续传: 临时文件已丢失，跳过: {} ({})", task.path, task.temp_path);
            db.remove_download_task(task.path);
            continue;
        }

        // 重建 ReceivingFile
        auto rf = std::make_shared<ReceivingFile>();
        rf->received_bitmap = deserialize_bitmap(task.bitmap_data, task.total_chunks);
        rf->total_chunks = task.total_chunks;

        // 从 bitmap 恢复 received_chunks 计数
        uint32_t count = 0;
        for (bool b : rf->received_bitmap) {
            if (b) count++;
        }
        rf->received_chunks = count;

        rf->temp_path = task.temp_path;
        rf->peer_id = task.peer_id;
        rf->expected_hash = task.expected_hash;
        rf->expected_size = task.expected_size;
        rf->last_active = std::chrono::steady_clock::now();
        rf->last_tick_chunks = count;       // 避免恢复后首次速度计算异常
        rf->last_tick_time = std::chrono::steady_clock::now();

        m_receiving_files[task.path] = std::move(rf);
        m_session_total++;
        restored++;
        g_logger->info("[Transfer] 断点续传: 已恢复下载任务: {} ({}/{})",
                       task.path, count, task.total_chunks);
    }

    if (restored > 0) {
        g_logger->info("[Transfer] 断点续传: 共恢复 {} 个下载任务", restored);
    }
}

}  // namespace VeritasSync