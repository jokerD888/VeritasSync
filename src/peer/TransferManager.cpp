#include "VeritasSync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/detail/socket_ops.hpp>  // for host_to_network/network_to_host
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <thread>

#include "VeritasSync/CryptoLayer.h"
#include "VeritasSync/EncodingUtils.h"
#include "VeritasSync/Hashing.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/Protocol.h"
#include "VeritasSync/StateManager.h"

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

    // 资源复用
    std::vector<char> buffer;
    std::string compressed_data;
    boost::asio::steady_timer timer;

    // 流控阈值
    const int CONGESTION_THRESHOLD = 4096;

    UploadContext(boost::asio::io_context& ioc) : timer(ioc), buffer(TransferManager::CHUNK_DATA_SIZE) {
        compressed_data.reserve(TransferManager::CHUNK_DATA_SIZE * 2);
    }
};

// --- 实现 ---

TransferManager::TransferManager(StateManager* sm, boost::asio::thread_pool& pool, CryptoLayer& crypto,
                                 SendCallback send_cb)
    : m_state_manager(sm), m_worker_pool(pool), m_crypto(crypto), m_send_callback(std::move(send_cb)) {}

void TransferManager::queue_upload(const std::string& peer_id, const nlohmann::json& request_payload) {
    if (peer_id.empty()) return;

    std::string requested_path_str = request_payload.at("path").get<std::string>();
    g_logger->info("[Transfer] 开始处理文件请求: {} -> {}", requested_path_str, peer_id);

    // 1. UI 占位
    {
        std::lock_guard<std::mutex> lock(m_transfer_mutex);
        m_sending_files[requested_path_str] = {0, 0};
    }

    if (!m_state_manager) return;

    // 创建上下文，持有 io_context 用于定时器
    // 注意：我们需要从 StateManager 获取 io_context
    auto ctx = std::make_shared<UploadContext>(m_state_manager->get_io_context());
    ctx->peer_id = peer_id;
    ctx->path = requested_path_str;

    // 2. 启动异步任务链
    boost::asio::post(m_worker_pool, [self = shared_from_this(), ctx]() {
        // --- 准备阶段 ---
        if (!self->m_state_manager) return;

        ctx->file_hash = self->m_state_manager->get_file_hash(ctx->path);
        std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(ctx->path);

        // 打开文件
        ctx->file.open(full_path, std::ios::binary | std::ios::ate);
        if (!ctx->file.is_open()) {
            g_logger->error("[Transfer] 无法打开文件: {}", full_path.string());
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files.erase(ctx->path);
            return;
        }

        std::streamsize size = ctx->file.tellg();
        ctx->file.seekg(0, std::ios::beg);

        // 计算块数 (0字节文件也算1块)
        ctx->total_chunks = (size > 0) ? static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE) : 1;

        // 更新 UI
        {
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files[ctx->path].total_chunks = static_cast<uint32_t>(ctx->total_chunks);
        }

        // --- 定义递归上传逻辑 ---
        auto upload_loop = [self, ctx](auto& loop_ref) -> void {
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

                // 加密
                std::string encrypted_msg = self->m_crypto.encrypt(binary_packet);
                if (encrypted_msg.empty()) break;  // 错误处理

                // 发送并获取积压量
                int pending = self->m_send_callback(ctx->peer_id, encrypted_msg);

                // 更新进度
                ctx->current_chunk++;
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    if (self->m_sending_files.count(ctx->path)) {
                        self->m_sending_files[ctx->path].sent_chunks = ctx->current_chunk;
                    }
                }

                // --- 智能流控 ---
                if (pending > ctx->CONGESTION_THRESHOLD) {
                    // 积压严重：设置定时器，异步等待，释放当前线程
                    // 动态调整休眠时间：积压越多，等待越久
                    int sleep_ms = (pending > ctx->CONGESTION_THRESHOLD * 1.5) ? 20 : 5;

                    ctx->timer.expires_after(std::chrono::milliseconds(sleep_ms));
                    ctx->timer.async_wait([self, ctx, loop_ref](const boost::system::error_code& ec) {
                        if (!ec) {
                            // 定时器回调在 IO 线程执行，我们需要跳回 Worker 线程池继续读取文件
                            boost::asio::post(self->m_worker_pool, [loop_ref]() {
                                // 递归调用，恢复 while 循环
                                loop_ref(loop_ref);
                            });
                        }
                    });
                    return;  // 退出当前函数，释放线程
                }
                // 如果没有积压，continue while 循环，直接发下一块，效率最高
            }

            // --- 循环结束：清理与收尾 ---
            if (ctx->current_chunk >= ctx->total_chunks) {
                if (!ctx->file_hash.empty()) {
                    self->m_state_manager->record_file_sent(ctx->peer_id, ctx->path, ctx->file_hash);
                }
                {
                    std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                    self->m_sending_files.erase(ctx->path);
                }
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
        if (it == self->m_receiving_files.end()) {
            // 新下载任务
            std::filesystem::path relative_path = Utf8ToPath(file_path_str);
            std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

            if (full_path.has_parent_path()) {
                std::filesystem::create_directories(full_path.parent_path());
            }

            std::filesystem::path temp_path = full_path;
            temp_path += ".veritas_tmp";

            ReceivingFile new_file;
            new_file.temp_path = temp_path.string();
            new_file.total_chunks = total_chunks;
            new_file.file_stream.open(temp_path, std::ios::binary | std::ios::out);

            if (!new_file.file_stream.is_open()) {
                g_logger->error("[Transfer] 无法创建临时文件: {}", temp_path.string());
                return;
            }

            auto res = self->m_receiving_files.insert({file_path_str, std::move(new_file)});
            it = res.first;
            g_logger->info("[Transfer] 开始接收: {} ({} 块)", file_path_str, total_chunks);
        }

        ReceivingFile& recv_file = it->second;
        recv_file.last_active = std::chrono::steady_clock::now();

        if (recv_file.file_stream.is_open()) {
            size_t offset = static_cast<size_t>(chunk_index) * CHUNK_DATA_SIZE;
            recv_file.file_stream.seekp(offset);
            recv_file.file_stream.write(uncompressed_data.data(), uncompressed_data.size());
            recv_file.received_chunks++;
        }

        // 4. 检查完成
        if (recv_file.received_chunks >= recv_file.total_chunks) {
            recv_file.file_stream.close();

            std::filesystem::path relative_path = Utf8ToPath(file_path_str);
            std::filesystem::path final_path = self->m_state_manager->get_root_path() / relative_path;

            std::error_code ec;
            std::filesystem::rename(recv_file.temp_path, final_path, ec);

            if (!ec) {
                g_logger->info("[Transfer] ✅ 下载完成: {}", file_path_str);
                if (!peer_id.empty()) {
                    std::string new_hash = Hashing::CalculateSHA256(final_path);
                    self->m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
                }
            } else {
                g_logger->error("[Transfer] 重命名失败: {}", ec.message());
            }

            self->m_receiving_files.erase(it);
        }
    });
}

std::vector<TransferStatus> TransferManager::get_active_transfers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::vector<TransferStatus> list;
    auto now = std::chrono::steady_clock::now();

    // 1. 处理下载任务 (Receiving)
    for (auto& [path, recv] : m_receiving_files) {
        // --- 速度计算逻辑 ---
        std::chrono::duration<double> elapsed = now - recv.last_tick_time;
        if (elapsed.count() >= 0.5) {  // 每 500ms 更新一次速度，避免抖动
            uint32_t delta_chunks = recv.received_chunks - recv.last_tick_chunks;
            // 速度 = (新增块数 * 块大小) / 经过时间
            recv.current_speed = (delta_chunks * CHUNK_DATA_SIZE) / elapsed.count();

            // 更新快照
            recv.last_tick_chunks = recv.received_chunks;
            recv.last_tick_time = now;
        }

        float prog =
            (recv.total_chunks > 0) ? (static_cast<float>(recv.received_chunks) / recv.total_chunks * 100.0f) : 0.0f;

        // 将 recv.current_speed 填入 TransferStatus
        list.push_back({path, recv.total_chunks, recv.received_chunks, prog, false, recv.current_speed});
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

        float prog =
            (send.total_chunks > 0) ? (static_cast<float>(send.sent_chunks) / send.total_chunks * 100.0f) : 0.0f;

        // 将 send.current_speed 填入 TransferStatus
        list.push_back({path, send.total_chunks, send.sent_chunks, prog, true, send.current_speed});
    }

    return list;
}

void TransferManager::cleanup_stale_buffers() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = m_receiving_files.begin(); it != m_receiving_files.end();) {
        auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - it->second.last_active);
        if (duration.count() > 10) {  // 10分钟超时
            g_logger->warn("[Transfer] 接收超时，清理: {}", it->first);
            if (it->second.file_stream.is_open()) it->second.file_stream.close();
            std::filesystem::remove(it->second.temp_path);
            it = m_receiving_files.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace VeritasSync