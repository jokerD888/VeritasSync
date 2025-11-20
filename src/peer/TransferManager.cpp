#include "VeritasSync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/detail/socket_ops.hpp>  // for host_to_network/network_to_host

#include "VeritasSync/CryptoLayer.h"
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

// --- 实现 ---

TransferManager::TransferManager(StateManager* sm, boost::asio::thread_pool& pool, CryptoLayer& crypto,
                                 SendCallback send_cb)
    : m_state_manager(sm), m_worker_pool(pool), m_crypto(crypto), m_send_callback(std::move(send_cb)) {}

void TransferManager::queue_upload(const std::string& peer_id, const nlohmann::json& request_payload) {
    if (peer_id.empty()) return;

    std::string requested_path_str = request_payload.at("path").get<std::string>();
    g_logger->info("[Transfer] 开始处理文件请求: {} -> {}", requested_path_str, peer_id);

    // 异步投递到线程池
    boost::asio::post(m_worker_pool, [self = shared_from_this(), requested_path_str, peer_id]() {
        // [Worker线程] 1. 获取 Hash 用于记录 (需确保 StateManager::get_file_hash 是线程安全的)
        std::string file_hash = self->m_state_manager->get_file_hash(requested_path_str);

        // [Worker线程] 2. 准备路径
        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(requested_path_str.c_str()), requested_path_str.length()));
        std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

        if (!std::filesystem::exists(full_path)) {
            g_logger->error("[Transfer] 文件不存在: {}", full_path.string());
            return;
        }

        std::ifstream file(full_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            g_logger->error("[Transfer] 无法打开文件: {}", full_path.string());
            return;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        int total_chunks = (size > 0) ? static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE) : 1;

        // 发送单个块的 Lambda
        auto send_chunk = [&](int index, const std::string& chunk_data) {
            // 组装协议头 (PathLen + Path + ChunkIdx + TotalChunks + Data)
            std::string packet_payload;
            // 预分配大致空间以减少重分配 (Header约 4+N+4+4 = 12+N bytes)
            packet_payload.reserve(12 + requested_path_str.length() + chunk_data.length());

            append_uint16(packet_payload, static_cast<uint16_t>(requested_path_str.length()));
            packet_payload.append(requested_path_str);
            append_uint32(packet_payload, index);
            append_uint32(packet_payload, total_chunks);
            packet_payload.append(chunk_data);

            // 加上类型前缀
            std::string binary_packet;
            binary_packet.reserve(1 + packet_payload.length());
            binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
            binary_packet.append(std::move(packet_payload));

            // [Worker线程] 加密
            std::string encrypted_msg = self->m_crypto.encrypt(binary_packet);
            if (encrypted_msg.empty()) return;

            // 回调 P2PManager 发送
            self->m_send_callback(peer_id, encrypted_msg);
        };

        // 处理空文件
        if (size == 0) {
            send_chunk(0, "");
            // [新增] 记录空文件发送历史
            self->m_state_manager->record_file_sent(peer_id, requested_path_str, file_hash);
            return;
        }

        // [Worker线程] 3. 循环处理 (Lite Optimization: 内存复用)
        // 预分配 buffer 和 compressed_data，避免循环内反复 malloc/free
        std::vector<char> buffer(CHUNK_DATA_SIZE);
        std::string compressed_data;
        compressed_data.reserve(CHUNK_DATA_SIZE * 2);  // 预留足够的压缩空间

        for (int i = 0; i < total_chunks; ++i) {
            file.read(buffer.data(), CHUNK_DATA_SIZE);
            std::streamsize bytes_read = file.gcount();

            // Snappy Compress 会重用 compressed_data 的容量，而不是每次都重新分配
            snappy::Compress(buffer.data(), bytes_read, &compressed_data);

            send_chunk(i, compressed_data);
        }

        // 4. 发送完成后，记录到 sync_history
        if (!file_hash.empty()) {
            self->m_state_manager->record_file_sent(peer_id, requested_path_str, file_hash);
            // g_logger->debug("[Transfer] 已记录发送历史: {} -> {}", requested_path_str, peer_id);
        }

        g_logger->debug("[Transfer] 文件发送完成: {}", requested_path_str);
    });
}
void TransferManager::handle_chunk(const std::string& payload, const std::string& peer_id) {
    // 此函数在主线程被调用 (从 P2PManager 收到数据后)
    // 为了不阻塞主线程解压和写盘，我们把它 throw 到 Worker 线程
    // 注意：payload 是 const ref，我们需要拷贝一份扔进 lambda
    // [修改] 将 peer_id 捕获进 lambda
    boost::asio::post(m_worker_pool, [self = shared_from_this(), payload, peer_id]() {
        const char* data_ptr = payload.c_str();
        size_t data_len = payload.length();

        // 1. 解析头部
        uint16_t path_len = read_uint16(data_ptr, data_len);
        if (path_len == 0 || data_len < path_len) return;  // 格式错误

        std::string file_path_str(data_ptr, path_len);
        data_ptr += path_len;
        data_len -= path_len;

        uint32_t chunk_index = read_uint32(data_ptr, data_len);
        uint32_t total_chunks = read_uint32(data_ptr, data_len);

        // 2. 解压 (CPU 密集)
        std::string uncompressed_data;
        if (data_len > 0) {
            if (!snappy::Uncompress(data_ptr, data_len, &uncompressed_data)) {
                g_logger->error("[Transfer] Snappy 解压失败: {}", file_path_str);
                return;
            }
        }

        // 3. 写入文件 (IO 密集) - 需要加锁保护 map
        std::lock_guard<std::mutex> lock(self->m_transfer_mutex);

        auto it = self->m_receiving_files.find(file_path_str);
        if (it == self->m_receiving_files.end()) {
            // 新下载任务
            std::filesystem::path relative_path(
                std::u8string_view(reinterpret_cast<const char8_t*>(file_path_str.c_str()), file_path_str.length()));
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

            std::filesystem::path relative_path(
                std::u8string_view(reinterpret_cast<const char8_t*>(file_path_str.c_str()), file_path_str.length()));
            std::filesystem::path final_path = self->m_state_manager->get_root_path() / relative_path;

            std::error_code ec;
            std::filesystem::rename(recv_file.temp_path, final_path, ec);

            if (!ec) {
                g_logger->info("[Transfer] ✅ 下载完成: {}", file_path_str);

                // [新增] 核心逻辑：更新 Base Hash (Sync History)
                // 只有记录了这一次同步的状态，下次发生修改时，我们才能正确判断是“冲突”还是“正常更新”
                if (!peer_id.empty()) {
                    // 重新计算落地文件的 Hash，确保数据一致性
                    std::string new_hash = Hashing::CalculateSHA256(final_path);
                    // 记录到数据库: "我和 peer_id 在 file_path_str 上达成了一致，hash 是 new_hash"
                    self->m_state_manager->record_sync_success(peer_id, file_path_str, new_hash);
                }
            } else {
                g_logger->error("[Transfer] 重命名失败: {}", ec.message());
            }

            self->m_receiving_files.erase(it);
        }
    });
}

std::vector<TransferStatus> TransferManager::get_active_downloads() {
    std::lock_guard<std::mutex> lock(m_transfer_mutex);
    std::vector<TransferStatus> list;
    for (const auto& [path, recv] : m_receiving_files) {
        float prog =
            (recv.total_chunks > 0) ? (static_cast<float>(recv.received_chunks) / recv.total_chunks * 100.0f) : 0.0f;
        list.push_back({path, recv.total_chunks, recv.received_chunks, prog});
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