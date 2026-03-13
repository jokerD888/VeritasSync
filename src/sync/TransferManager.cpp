#include "VeritasSync/sync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cerrno>
#include <thread>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/common/PathUtils.h"
#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"

namespace VeritasSync {

// E-1: 魔数统一为命名常量
static constexpr int  FILE_OPEN_MAX_RETRIES       = 5;      // 文件打开最大重试次数
static constexpr int  FILE_OPEN_RETRY_DELAY_MS    = 200;    // 文件打开失败重试延迟（毫秒）
static constexpr int  FILE_OPEN_EMFILE_DELAY_MS   = 1000;   // 文件句柄耗尽时重试延迟（毫秒）
static constexpr int  STALL_THRESHOLD_MS          = 5000;   // 传输停滞判定阈值（毫秒）
static constexpr int  ZOMBIE_THRESHOLD_SECONDS    = 10;     // 僵尸任务判定阈值（秒）
static constexpr int  RECEIVE_TIMEOUT_MINUTES     = 10;     // 接收超时关闭文件流阈值（分钟）
static constexpr int  CONGESTION_WAIT_HIGH_MS     = 200;    // 高积压时流控等待（毫秒）
static constexpr int  CONGESTION_WAIT_LOW_MS      = 100;    // 低积压时流控等待（毫秒）
static constexpr int  CONGESTION_HIGH_MULTIPLIER  = 4;      // 高积压判定倍数
static constexpr double SPEED_UPDATE_INTERVAL_SEC = 0.5;    // 速度更新间隔（秒）

// --- 上传任务上下文 (用于异步管理状态) ---
struct UploadContext {
    std::string peer_id;
    std::string path;
    std::ifstream file;
    std::string file_hash;
    int total_chunks = 0;
    int current_chunk = 0;
    int open_retry_count = 0; // 新增：用于异步重试计数

    // 资源复用
    std::vector<char> buffer;
    std::string compressed_data;
    boost::asio::steady_timer timer;

    // 流控阈值：当 KCP 发送队列超过此值时触发流控等待
    // 注意：旧版本 (b1148f9) 使用 1024，新版本因锁层次增加，需要更早触发流控
    static constexpr int CONGESTION_THRESHOLD = 256;

    // 运行时 chunk 大小
    size_t chunk_data_size;

    UploadContext(boost::asio::io_context& ioc, size_t chunk_size) 
        : timer(ioc), buffer(chunk_size), chunk_data_size(chunk_size) {
        compressed_data.reserve(chunk_size * 2);
    }
};

// --- 实现 ---

TransferManager::SessionStats TransferManager::get_session_stats() const {
    return {m_session_total.load(), m_session_done.load()};
}
TransferManager::TransferManager(StateManager* sm, boost::asio::io_context& io_context,
                                 boost::asio::thread_pool& pool, SendCallback send_cb,
                                 size_t chunk_size)
    : m_state_manager(sm), m_io_context(io_context), m_worker_pool(pool), m_send_callback(std::move(send_cb)),
      CHUNK_DATA_SIZE(chunk_size) {}

void TransferManager::queue_upload(const std::string& peer_id, const nlohmann::json& request_payload) {
    if (peer_id.empty()) return;

    std::string requested_path_str = request_payload.at("path").get<std::string>();
    
    // 【断点续传】解析可选参数
    uint32_t start_chunk = request_payload.value("start_chunk", 0u);
    std::string expected_hash = request_payload.value("expected_hash", "");
    uint64_t expected_size = request_payload.value("expected_size", 0ull);
    
    if (start_chunk > 0) {
        g_logger->info("[Transfer] 收到续传请求: {} -> {} (start_chunk={})", 
                      requested_path_str, peer_id, start_chunk);
    } else {
        g_logger->info("[Transfer] 开始处理文件请求: {} -> {}", requested_path_str, peer_id);
    }

    // 1. UI 占位
    {
        std::lock_guard<std::mutex> lock(m_transfer_mutex);
        m_sending_files[requested_path_str] = {0, 0};
        m_session_total++;
    }

    if (!m_state_manager) return;

    // 创建上下文，持有 io_context 用于定时器
    auto ctx = std::make_shared<UploadContext>(m_io_context, CHUNK_DATA_SIZE);
    ctx->peer_id = peer_id;
    ctx->path = requested_path_str;

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
                self->m_sending_files.erase(ctx->path);
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
        ctx->current_chunk = static_cast<int>(actual_start_chunk);

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
                    int wait_ms = (errno == EMFILE || errno == ENFILE) ? FILE_OPEN_EMFILE_DELAY_MS : FILE_OPEN_RETRY_DELAY_MS;
                    
                    if (ctx->open_retry_count < FILE_OPEN_MAX_RETRIES) {
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
                        g_logger->error("[Transfer] ❌ 无法打开文件(重试{}次后放弃): {} | {}", FILE_OPEN_MAX_RETRIES, ctx->path, sys_err);
                        {
                            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                            self->m_sending_files.erase(ctx->path);
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
                    ctx->total_chunks = (size > 0) ? static_cast<int>((size + ctx->chunk_data_size - 1) / ctx->chunk_data_size) : 1;
                    
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    if (self->m_sending_files.count(ctx->path)) {
                        self->m_sending_files[ctx->path].total_chunks = static_cast<uint32_t>(ctx->total_chunks);
                    }
                }
                
                // 定位并继续
                ctx->file.seekg(static_cast<std::streampos>(ctx->current_chunk) * ctx->chunk_data_size, std::ios::beg);
            }
            // 使用 while 循环来处理非阻塞的情况，提高效率
            while (ctx->current_chunk < ctx->total_chunks) {
                // 读取数据
                ctx->file.read(ctx->buffer.data(), ctx->chunk_data_size);
                std::streamsize bytes_read = ctx->file.gcount();

                // 压缩
                // 优化：如果是 0 字节，Snappy 处理为空串，正常兼容
                snappy::Compress(ctx->buffer.data(), bytes_read, &ctx->compressed_data);

                // 组包
                std::string packet_payload;
                // 预估大小：PathLen(2) + Path(N) + ChunkIdx(4) + TotalChunks(4) + CompressedData(M)
                static constexpr size_t CHUNK_HEADER_OVERHEAD = 2 + 4 + 4;  // PathLen + ChunkIdx + TotalChunks
                packet_payload.reserve(CHUNK_HEADER_OVERHEAD + ctx->path.length() + ctx->compressed_data.length());

                append_uint16(packet_payload, static_cast<uint16_t>(ctx->path.length()));
                packet_payload.append(ctx->path);
                append_uint32(packet_payload, ctx->current_chunk);
                append_uint32(packet_payload, ctx->total_chunks);
                packet_payload.append(ctx->compressed_data);

                std::string binary_packet;
                binary_packet.reserve(1 + packet_payload.length());
                binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
                binary_packet.append(std::move(packet_payload));

                // 加密已下沉到 PeerController (KCP 层)
                // 这里直接发送明文数据包
                int pending = self->m_send_callback(ctx->peer_id, binary_packet);

                // 【断点续传】检测连接是否已断开
                if (pending < 0) {
                    g_logger->warn("[Transfer] 连接已断开，终止发送: {} (已发送 {}/{} 块)", 
                                  ctx->path, ctx->current_chunk, ctx->total_chunks);
                    ctx->file.close();
                    {
                        std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                        self->m_sending_files.erase(ctx->path);
                    }
                    self->m_session_done++;
                    return;  // 提前退出，避免 CPU 浪费
                }

                // 更新进度
                ctx->current_chunk++;
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    if (self->m_sending_files.count(ctx->path)) {
                        self->m_sending_files[ctx->path].sent_chunks = ctx->current_chunk;
                        // 更新活跃时间 (喂狗)
                        self->m_sending_files[ctx->path].last_active = std::chrono::steady_clock::now();
                    }
                }

                // --- 智能流控 ---
                if (pending > ctx->CONGESTION_THRESHOLD) {
                    // 【关键修复】在挂起等待前，主动关闭文件句柄！
                    // 这确保了只有当前正在活跃读写的线程（通常只有几个）会占用句柄。
                    ctx->file.close();

                    // 增加等待时间，给 KCP 更多时间消耗发送队列
                    // 新版本锁层次增加，需要更长的等待时间
                    int sleep_ms = (pending > ctx->CONGESTION_THRESHOLD * CONGESTION_HIGH_MULTIPLIER) ? CONGESTION_WAIT_HIGH_MS : CONGESTION_WAIT_LOW_MS;
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
                if (!ctx->file_hash.empty()) {
                    self->m_state_manager->record_file_sent(ctx->peer_id, ctx->path, ctx->file_hash);
                }
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    self->m_sending_files.erase(ctx->path);
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
// C-2 超长函数拆分：handle_chunk 的子步骤
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

    // 【安全】路径遍历攻击防护：验证网络传入的路径不会逃逸出同步根目录
    if (!PathUtils::is_path_safe(m_state_manager->get_root_path(), file_path_str)) {
        g_logger->error("[Transfer] ⚠️ 路径安全检查失败，拒绝接收: {}", file_path_str);
        return result;  // result.recv_ptr 为 nullptr，上层会跳过
    }

    std::lock_guard<std::mutex> map_lock(m_transfer_mutex);

    auto it = m_receiving_files.find(file_path_str);
    bool is_new_task = (it == m_receiving_files.end());
    result.need_open_stream = is_new_task;

    if (is_new_task) {
        auto new_file = std::make_shared<ReceivingFile>();

        std::filesystem::path relative_path = Utf8ToPath(file_path_str);
        result.full_path = m_state_manager->get_root_path() / relative_path;
        result.temp_path = result.full_path;
        result.temp_path += ".veritas_tmp";

        new_file->temp_path = PathToUtf8(result.temp_path);
        new_file->total_chunks = total_chunks;
        // 【修复】初始化 bitmap 用于去重
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
        }

        if (result.recv_ptr->temp_path.empty()) {
            std::filesystem::path relative_path = Utf8ToPath(file_path_str);
            result.full_path = m_state_manager->get_root_path() / relative_path;
            result.temp_path = result.full_path;
            result.temp_path += ".veritas_tmp";
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

    recv_ptr->file_stream.close();

    std::filesystem::path relative_path = Utf8ToPath(file_path_str);
    std::filesystem::path final_path = m_state_manager->get_root_path() / relative_path;
    std::filesystem::path temp_path_obj = Utf8ToPath(recv_ptr->temp_path);

    std::error_code ec;
    std::filesystem::rename(temp_path_obj, final_path, ec);

    if (!ec) {
        g_logger->info("[Transfer] ✅ 下载完成: {}", file_path_str);
        if (!peer_id.empty()) {
            std::string new_hash = Hashing::CalculateSHA256(final_path);
            m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
            m_state_manager->mark_file_received(file_path_str, new_hash);
        }
    } else {
        g_logger->error("[Transfer] 重命名失败: {} -> {} | {}", recv_ptr->temp_path, PathToUtf8(final_path), ec.message());
    }
    m_session_done++;

    // 短暂全局锁，从 map 中移除已完成的条目
    std::lock_guard<std::mutex> map_lock(m_transfer_mutex);
    m_receiving_files.erase(file_path_str);
}

// ═══════════════════════════════════════════════════════════════

void TransferManager::handle_chunk(const std::string& payload, const std::string& peer_id) {
    boost::asio::post(m_worker_pool, [self = shared_from_this(), payload, peer_id]() {
        std::shared_ptr<ReceivingFile> recv_ptr;  // 提到 try 外部，供 catch 块清理用
        try {
            // 阶段 0: 解析头部 + Snappy 解压（无锁）
            auto hdr = parse_chunk_payload(payload);
            if (!hdr.valid) return;

            // 阶段 A: 短暂全局锁，查找/创建 ReceivingFile
            auto lookup = self->lookup_or_create_receiving(hdr.file_path, hdr.total_chunks, peer_id);
            recv_ptr = lookup.recv_ptr;

            // 阶段 B: per-file 锁，执行所有 I/O 操作
            std::lock_guard<std::mutex> file_lock(recv_ptr->file_mutex);

            auto now = std::chrono::steady_clock::now();

            // 僵尸复活检测
            auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - recv_ptr->last_active).count();
            if (duration_sec > ZOMBIE_THRESHOLD_SECONDS) {
                if (hdr.chunk_index == 0) {
                    g_logger->warn("[Transfer] 检测到僵尸任务复活 (重启): {}, 重置进度。", hdr.file_path);
                    recv_ptr->received_chunks = 0;
                    // 【修复】重置 bitmap
                    std::fill(recv_ptr->received_bitmap.begin(), recv_ptr->received_bitmap.end(), false);
                } else {
                    g_logger->info("[Transfer] 检测到僵尸任务恢复 (断网重连): {}", hdr.file_path);
                }
            }
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

            // 【修复】使用 bitmap 去重：如果这个 chunk 已经收过，跳过写入
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
                std::lock_guard<std::mutex> map_lock(self->m_transfer_mutex);
                self->m_receiving_files.erase(hdr.file_path);
                return;
            }
            // 【修复】标记 bitmap 并递增计数器（仅新 chunk 时）
            recv_ptr->received_bitmap[hdr.chunk_index] = true;
            recv_ptr->received_chunks++;

            // 阶段 C: 检查完成 → 调用收尾方法
            if (recv_ptr->received_chunks >= recv_ptr->total_chunks) {
                self->finalize_received_file(hdr.file_path, recv_ptr, peer_id);
            } else {
                recv_ptr->busy = false;
            }
        } catch (const std::exception& e) {
            g_logger->error("[Transfer] handle_chunk 异常: {}", e.what());
            // 【修复】异常路径下必须重置 busy 标志并关闭文件流，
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


    // 1. 处理下载任务 (Receiving)
    for (auto& [path, recv_ptr] : m_receiving_files) {
        auto& recv = *recv_ptr;
        // --- 速度计算逻辑 ---
        std::chrono::duration<double> elapsed = now - recv.last_tick_time;
        if (elapsed.count() >= SPEED_UPDATE_INTERVAL_SEC) {  // 每 500ms 更新一次速度
            uint32_t delta_chunks = recv.received_chunks - recv.last_tick_chunks;
            recv.current_speed = (delta_chunks * CHUNK_DATA_SIZE) / elapsed.count();
            recv.last_tick_chunks = recv.received_chunks;
            recv.last_tick_time = now;
        }

        // 【新增】检测是否停滞
        auto time_since_active = std::chrono::duration_cast<std::chrono::milliseconds>(now - recv.last_active).count();
        bool stalled = (time_since_active > STALL_THRESHOLD_MS);

        // 如果停滞，速度显示为 0，避免用户困惑
        double display_speed = stalled ? 0.0 : recv.current_speed;

        float prog =
            (recv.total_chunks > 0) ? (static_cast<float>(recv.received_chunks) / recv.total_chunks * 100.0f) : 0.0f;

        // 【修改】填入 stalled 状态
        list.push_back({
            path, recv.total_chunks, recv.received_chunks, prog,
            false,  // is_upload
            display_speed,
            stalled  // stalled
        });
    }

    // 2. 处理上传任务 (Sending)
    for (auto& [path, send] : m_sending_files) {
        // --- 速度计算逻辑 ---
        std::chrono::duration<double> elapsed = now - send.last_tick_time;
        if (elapsed.count() >= SPEED_UPDATE_INTERVAL_SEC) {
            uint32_t delta_chunks = send.sent_chunks - send.last_tick_chunks;
            send.current_speed = (delta_chunks * CHUNK_DATA_SIZE) / elapsed.count();
            send.last_tick_chunks = send.sent_chunks;
            send.last_tick_time = now;
        }

        // 【新增】检测是否停滞
        auto time_since_active = std::chrono::duration_cast<std::chrono::milliseconds>(now - send.last_active).count();
        bool stalled = (time_since_active > STALL_THRESHOLD_MS);

        double display_speed = stalled ? 0.0 : send.current_speed;

        float prog =
            (send.total_chunks > 0) ? (static_cast<float>(send.sent_chunks) / send.total_chunks * 100.0f) : 0.0f;

        // 【修改】填入 stalled 状态
        list.push_back({
            path, send.total_chunks, send.sent_chunks, prog,
            true,  // is_upload
            display_speed,
            stalled  // stalled
        });
    }

    return list;
}

void TransferManager::cleanup_stale_buffers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = m_receiving_files.begin(); it != m_receiving_files.end(); ++it) {
        auto& recv = *it->second;
        // C-2: 如果条目正在被 handle_chunk 处理，跳过
        if (recv.busy) continue;
        
        // 接收超时：只关闭文件流，保留记录和临时文件以便续传
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - recv.last_active);
        if (duration.count() > RECEIVE_TIMEOUT_MINUTES) {
            if (recv.file_stream.is_open()) {
                g_logger->warn("[Transfer] ⏰ 接收超时，关闭文件流（保留状态等待续传）: {}", it->first);
                recv.file_stream.close();
            }
            // 【断点续传】不删除记录和临时文件，由 cancel_receives_for_peer 或后续校验决定
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// 断点续传相关方法
// ═══════════════════════════════════════════════════════════════

void TransferManager::cancel_receives_for_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    
    int cancelled_count = 0;
    for (auto it = m_receiving_files.begin(); it != m_receiving_files.end(); ) {
        auto& recv = *it->second;
        if (recv.peer_id == peer_id) {
            // C-2: 如果 handle_chunk 正在处理这个条目，跳过
            // handle_chunk 会在完成后自行清理
            if (recv.busy) {
                g_logger->info("[Transfer] 任务 {} 正在处理中，跳过清理", it->first);
                ++it;
                continue;
            }
            
            g_logger->info("[Transfer] 清理来自 {} 的未完成任务: {} ({}/{})", 
                          peer_id, it->first, 
                          recv.received_chunks, recv.total_chunks);
            
            // 关闭文件流
            if (recv.file_stream.is_open()) {
                recv.file_stream.close();
            }
            
            // 删除临时文件
            std::error_code ec;
            std::filesystem::remove(recv.temp_path, ec);
            if (ec) {
                g_logger->warn("[Transfer] 删除临时文件失败: {} | {}", 
                              recv.temp_path, FormatErrorCode(ec));
            }
            
            it = m_receiving_files.erase(it);
            cancelled_count++;
        } else {
            ++it;
        }
    }
    
    if (cancelled_count > 0) {
        g_logger->info("[Transfer] 已清理 {} 的 {} 个未完成任务", peer_id, cancelled_count);
    }
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
    g_logger->info("[Transfer] ✅ 可以续传: {} ({}/{} chunks, {:.1f}%)", 
                  path, rf.received_chunks, rf.total_chunks,
                  rf.total_chunks > 0 ? (100.0 * rf.received_chunks / rf.total_chunks) : 0.0);
    
    return ResumeInfo{
        path,
        rf.received_chunks,
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
        // 更新已有记录的元数据
        auto& rf = *it->second;
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

}  // namespace VeritasSync