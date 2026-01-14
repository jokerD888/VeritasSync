#include "VeritasSync/sync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/detail/socket_ops.hpp>  // for host_to_network/network_to_host
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <thread>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"

namespace VeritasSync {

// --- 辅助函数 (协议封包解包) ---
static void append_uint16(std::string& s, uint16_t val) {
    uint16_t net_val = boost::asio::detail::socket_ops::host_to_network_short(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}
static void append_uint32(std::string& s, uint32_t val) {
    uint32_t net_val = boost::asio::detail::socket_ops::host_to_network_long(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}
static uint16_t read_uint16(const char*& data, size_t& len) {
    if (len < sizeof(uint16_t)) return 0;
    uint16_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_short(net_val);
}
static uint32_t read_uint32(const char*& data, size_t& len) {
    if (len < sizeof(uint32_t)) return 0;
    uint32_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_long(net_val);
}
static const uint8_t MSG_TYPE_BINARY_CHUNK = 0x02;

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
    const int CONGESTION_THRESHOLD = 256;

    UploadContext(boost::asio::io_context& ioc) : timer(ioc), buffer(TransferManager::CHUNK_DATA_SIZE) {
        compressed_data.reserve(TransferManager::CHUNK_DATA_SIZE * 2);
    }
};

// --- 实现 ---

TransferManager::SessionStats TransferManager::get_session_stats() const {
    return {m_session_total.load(), m_session_done.load()};
}
TransferManager::TransferManager(StateManager* sm, boost::asio::thread_pool& pool, SendCallback send_cb)
    : m_state_manager(sm), m_worker_pool(pool), m_send_callback(std::move(send_cb)) {}

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
    // 注意：我们需要从 StateManager 获取 io_context
    auto ctx = std::make_shared<UploadContext>(m_state_manager->get_io_context());
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
                    int wait_ms = (errno == 24 || errno == 23) ? 1000 : 200;
                    
                    if (ctx->open_retry_count < 5) {
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
                        g_logger->error("[Transfer] ❌ 无法打开文件(重试5次后放弃): {} | {}", ctx->path, sys_err);
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
                    ctx->total_chunks = (size > 0) ? static_cast<int>((size + TransferManager::CHUNK_DATA_SIZE - 1) / TransferManager::CHUNK_DATA_SIZE) : 1;
                    
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    if (self->m_sending_files.count(ctx->path)) {
                        self->m_sending_files[ctx->path].total_chunks = static_cast<uint32_t>(ctx->total_chunks);
                    }
                }
                
                // 定位并继续
                ctx->file.seekg(static_cast<std::streampos>(ctx->current_chunk) * TransferManager::CHUNK_DATA_SIZE, std::ios::beg);
            }
            // 使用 while 循环来处理非阻塞的情况，提高效率
            while (ctx->current_chunk < ctx->total_chunks) {
                // 读取数据
                ctx->file.read(ctx->buffer.data(), CHUNK_DATA_SIZE);
                std::streamsize bytes_read = ctx->file.gcount();

                // 压缩
                // 优化：如果是 0 字节，Snappy 处理为空串，正常兼容
                snappy::Compress(ctx->buffer.data(), bytes_read, &ctx->compressed_data);

                // 组包
                std::string packet_payload;
                // 预估大小：PathLen(2) + Path(N) + Idx(4) + Total(4) + Data(M)
                packet_payload.reserve(12 + ctx->path.length() + ctx->compressed_data.length());

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
                    int sleep_ms = (pending > ctx->CONGESTION_THRESHOLD * 4) ? 200 : 100;
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
void TransferManager::handle_chunk(const std::string& payload, const std::string& peer_id) {
    // 此函数在主线程被调用，Throw 到 Worker 线程
    boost::asio::post(m_worker_pool, [self = shared_from_this(), payload, peer_id]() {
        try {
            const char* data_ptr = payload.c_str();
            size_t data_len = payload.length();

            // 1. 解析头部
            uint16_t path_len = read_uint16(data_ptr, data_len);
            if (path_len == 0 || data_len < path_len) return;

            std::string file_path_str(data_ptr, path_len);
            data_ptr += path_len;
            data_len -= path_len;

            uint32_t chunk_index = read_uint32(data_ptr, data_len);
            uint32_t total_chunks = read_uint32(data_ptr, data_len);

            // 2. 解压
            std::string uncompressed_data;
            if (data_len > 0) {
                if (!snappy::Uncompress(data_ptr, data_len, &uncompressed_data)) {
                    g_logger->error("[Transfer] Snappy 解压失败: {}", file_path_str);
                    return;
                }
            }

            // 3. 写入文件 - 需要加锁保护 map
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);

            auto it = self->m_receiving_files.find(file_path_str);
            bool is_new_task = (it == self->m_receiving_files.end());
            bool need_open_stream = is_new_task;
            
            if (is_new_task) {
                // 新下载任务
                std::filesystem::path relative_path = Utf8ToPath(file_path_str);
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

                if (full_path.has_parent_path()) {
                    std::error_code dir_ec;
                    std::filesystem::create_directories(full_path.parent_path(), dir_ec);
                    if (dir_ec) {
                        g_logger->error("[Transfer] ❌ 无法创建目录: {} | {}", PathToUtf8(full_path.parent_path()), FormatErrorCode(dir_ec));
                        return;
                    }
                }

                std::filesystem::path temp_path = full_path;
                temp_path += ".veritas_tmp";

                ReceivingFile new_file;
                // 【修复】使用 PathToUtf8 确保 Windows 上中文路径正确编码
                new_file.temp_path = PathToUtf8(temp_path);
                new_file.total_chunks = total_chunks;
                new_file.peer_id = peer_id;  // 【断点续传】保存 peer_id

                auto res = self->m_receiving_files.insert({file_path_str, std::move(new_file)});
                it = res.first;
                self->m_session_total++;
                g_logger->info("[Transfer] 开始接收: {} ({} 块)", file_path_str, total_chunks);
            } else {
                // 已有记录（可能是预注册的或续传的）
                // 更新 peer_id 和 total_chunks
                it->second.peer_id = peer_id;
                if (it->second.total_chunks == 0) {
                    it->second.total_chunks = total_chunks;
                }
                
                // 如果 temp_path 为空（预注册状态），需要初始化
                if (it->second.temp_path.empty()) {
                    std::filesystem::path relative_path = Utf8ToPath(file_path_str);
                    std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;
                    if (full_path.has_parent_path()) {
                        std::error_code dir_ec;
                        std::filesystem::create_directories(full_path.parent_path(), dir_ec);
                    }
                    std::filesystem::path temp_path = full_path;
                    temp_path += ".veritas_tmp";
                    // 【修复】使用 PathToUtf8 确保 Windows 上中文路径正确编码
                    it->second.temp_path = PathToUtf8(temp_path);
                    need_open_stream = true;
                    self->m_session_total++;
                    g_logger->info("[Transfer] 开始接收（从预注册）: {} ({} 块)", file_path_str, total_chunks);
                }
            }

        ReceivingFile& recv_file = it->second;
        auto now = std::chrono::steady_clock::now();

        // 【新增 >>> 核心复活逻辑】
        // 检查是否为“僵尸复活”：如果距离上次活跃超过 10 秒
        auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - recv_file.last_active).count();
        if (duration_sec > 10) {
            // 情况 C：进程重启。特征：停滞很久，且收到的是起始块 (Chunk 0)
            if (chunk_index == 0) {
                g_logger->warn("[Transfer] 检测到僵尸任务复活 (重启): {}, 重置进度。", file_path_str);
                recv_file.received_chunks = 0;  // 重置计数器
            } else {
                // 情况 B：断网恢复。继续传输。
                g_logger->info("[Transfer] 检测到僵尸任务恢复 (断网重连): {}", file_path_str);
            }
        }
        // 【<<< 新增结束】

        // 【修改】使用 now 变量更新活跃时间 (喂狗)
        recv_file.last_active = now;

        // 【断点续传】如果文件流未打开，需要打开或重新打开
        if (!recv_file.file_stream.is_open()) {
            std::filesystem::path temp_path = Utf8ToPath(recv_file.temp_path);
            
            if (need_open_stream && !std::filesystem::exists(temp_path)) {
                // 新任务，创建新文件
                recv_file.file_stream.open(temp_path, std::ios::binary | std::ios::out);
            } else {
                // 续传任务，以读写模式打开现有文件
                recv_file.file_stream.open(temp_path, std::ios::binary | std::ios::in | std::ios::out);
            }
            
            if (!recv_file.file_stream.is_open()) {
                std::string sys_err = GetLastSystemError();
                g_logger->error("[Transfer] ❌ 无法打开临时文件: {} | {}", recv_file.temp_path, sys_err);
                return;
            }
            g_logger->debug("[Transfer] 打开文件流: {}", recv_file.temp_path);
        }
        
        // 写入数据
        size_t offset = static_cast<size_t>(chunk_index) * CHUNK_DATA_SIZE;
        recv_file.file_stream.seekp(offset);
        recv_file.file_stream.write(uncompressed_data.data(), uncompressed_data.size());
        
        // 检查写入是否成功
        if (recv_file.file_stream.fail()) {
            std::string sys_err = GetLastSystemError();
            g_logger->error("[Transfer] ❌ 文件写入失败 (chunk {}): {} | {}", chunk_index, file_path_str, sys_err);
            recv_file.file_stream.close();
            return;
        }
        recv_file.received_chunks++;

        // 4. 检查完成
        if (recv_file.received_chunks >= recv_file.total_chunks) {
            recv_file.file_stream.close();

            std::filesystem::path relative_path = Utf8ToPath(file_path_str);
            std::filesystem::path final_path = self->m_state_manager->get_root_path() / relative_path;
            // 【修复】使用 Utf8ToPath 转换 temp_path 以确保正确处理中文路径
            std::filesystem::path temp_path_obj = Utf8ToPath(recv_file.temp_path);

            std::error_code ec;
            std::filesystem::rename(temp_path_obj, final_path, ec);

            if (!ec) {
                g_logger->info("[Transfer] ✅ 下载完成: {}", file_path_str);
                if (!peer_id.empty()) {
                    std::string new_hash = Hashing::CalculateSHA256(final_path);
                    self->m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
                }
            } else {
                g_logger->error("[Transfer] 重命名失败: {} -> {} | {}", recv_file.temp_path, PathToUtf8(final_path), ec.message());
            }
            self->m_session_done++;
            self->m_receiving_files.erase(it);
        }
        } catch (const std::exception& e) {
            g_logger->error("[Transfer] handle_chunk 异常: {}", e.what());
        } catch (...) {
            g_logger->error("[Transfer] handle_chunk 未知异常");
        }
    });
}

std::vector<TransferStatus> TransferManager::get_active_transfers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::vector<TransferStatus> list;
    auto now = std::chrono::steady_clock::now();

    // 【新增】定义超时阈值 (5秒没数据就算停滞)
    const int STALL_THRESHOLD_MS = 5000;

    // 1. 处理下载任务 (Receiving)
    for (auto& [path, recv] : m_receiving_files) {
        // --- 速度计算逻辑 ---
        std::chrono::duration<double> elapsed = now - recv.last_tick_time;
        if (elapsed.count() >= 0.5) {  // 每 500ms 更新一次速度
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
        if (elapsed.count() >= 0.5) {
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
        // 10分钟超时：只关闭文件流，保留记录和临时文件以便续传
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.last_active);
        if (duration.count() > 10) {
            if (it->second.file_stream.is_open()) {
                g_logger->warn("[Transfer] ⏰ 接收超时，关闭文件流（保留状态等待续传）: {}", it->first);
                it->second.file_stream.close();
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
        if (it->second.peer_id == peer_id) {
            g_logger->info("[Transfer] 清理来自 {} 的未完成任务: {} ({}/{})", 
                          peer_id, it->first, 
                          it->second.received_chunks, it->second.total_chunks);
            
            // 关闭文件流
            if (it->second.file_stream.is_open()) {
                it->second.file_stream.close();
            }
            
            // 删除临时文件
            std::error_code ec;
            std::filesystem::remove(it->second.temp_path, ec);
            if (ec) {
                g_logger->warn("[Transfer] 删除临时文件失败: {} | {}", 
                              it->second.temp_path, FormatErrorCode(ec));
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
    
    ReceivingFile& rf = it->second;
    
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
        it->second.peer_id = peer_id;
        it->second.expected_hash = hash;
        it->second.expected_size = size;
        g_logger->debug("[Transfer] 更新接收任务元数据: {} (hash={}..., size={})", 
                       path, hash.substr(0, 8), size);
    } else {
        // 创建新的占位记录（actual file_stream 和 temp_path 在 handle_chunk 时填充）
        ReceivingFile rf;
        rf.peer_id = peer_id;
        rf.expected_hash = hash;
        rf.expected_size = size;
        rf.last_active = std::chrono::steady_clock::now();
        m_receiving_files[path] = std::move(rf);
        g_logger->debug("[Transfer] 预注册接收任务: {} (hash={}..., size={})", 
                       path, hash.substr(0, 8), size);
    }
}

}  // namespace VeritasSync