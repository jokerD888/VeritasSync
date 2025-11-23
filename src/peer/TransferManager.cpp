#include "VeritasSync/TransferManager.h"

#include <snappy.h>

#include <boost/asio/detail/socket_ops.hpp>  // for host_to_network/network_to_host
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

// --- 实现 ---

TransferManager::TransferManager(StateManager* sm, boost::asio::thread_pool& pool, CryptoLayer& crypto,
                                 SendCallback send_cb)
    : m_state_manager(sm), m_worker_pool(pool), m_crypto(crypto), m_send_callback(std::move(send_cb)) {}

void TransferManager::queue_upload(const std::string& peer_id, const nlohmann::json& request_payload) {
    if (peer_id.empty()) return;

    std::string requested_path_str = request_payload.at("path").get<std::string>();
    g_logger->info("[Transfer] 开始处理文件请求: {} -> {}", requested_path_str, peer_id);

    // 1. 提前在 Map 中占位 (防止 UI 闪烁，先置为 0/0)
    {
        std::lock_guard<std::mutex> lock(m_transfer_mutex);
        m_sending_files[requested_path_str] = {0, 0};
    }

    // 2. 异步投递到 Worker 线程池
    boost::asio::post(m_worker_pool, [self = shared_from_this(), requested_path_str, peer_id]() {
        if (!self->m_state_manager) return;

        // --- 准备阶段 ---
        std::string file_hash = self->m_state_manager->get_file_hash(requested_path_str);
        std::filesystem::path relative_path = Utf8ToPath(requested_path_str);
        std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

        if (!std::filesystem::exists(full_path)) {
            g_logger->error("[Transfer] 文件不存在: {}", full_path.string());
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files.erase(requested_path_str);
            return;
        }

        std::ifstream file(full_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            g_logger->error("[Transfer] 无法打开文件: {}", full_path.string());
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files.erase(requested_path_str);
            return;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        // 计算总块数
        int total_chunks = (size > 0) ? static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE) : 1;

        // 更新 UI 显示用的总块数
        {
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files[requested_path_str].total_chunks = static_cast<uint32_t>(total_chunks);
        }

        // --- 定义发送单个块的 Lambda ---
        auto send_chunk = [&](int index, const std::string& chunk_data) -> int {
            // 组装协议包 (PathLen + Path + ChunkIdx + TotalChunks + Data)
            std::string packet_payload;
            packet_payload.reserve(12 + requested_path_str.length() + chunk_data.length());

            append_uint16(packet_payload, static_cast<uint16_t>(requested_path_str.length()));
            packet_payload.append(requested_path_str);
            append_uint32(packet_payload, index);
            append_uint32(packet_payload, total_chunks);
            packet_payload.append(chunk_data);

            // 加上 MSG_TYPE 前缀
            std::string binary_packet;
            binary_packet.reserve(1 + packet_payload.length());
            binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
            binary_packet.append(std::move(packet_payload));

            // 加密
            std::string encrypted_msg = self->m_crypto.encrypt(binary_packet);
            if (encrypted_msg.empty()) return 0;

            // [关键] 调用回调发送，并获取当前 KCP 积压量
            // 因为 P2PManager 改为了同步发送，这里返回的 pending 是实时的
            int pending = self->m_send_callback(peer_id, encrypted_msg);

            // 更新 UI 进度
            {
                std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
                if (self->m_sending_files.count(requested_path_str)) {
                    self->m_sending_files[requested_path_str].sent_chunks++;
                }
            }

            return pending;
        };

        // 处理 0 字节空文件
        if (size == 0) {
            send_chunk(0, "");
            self->m_state_manager->record_file_sent(peer_id, requested_path_str, file_hash);
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files.erase(requested_path_str);
            return;
        }

        // --- 循环发送阶段 (带智能流控) ---
        std::vector<char> buffer(CHUNK_DATA_SIZE);
        std::string compressed_data;
        compressed_data.reserve(CHUNK_DATA_SIZE * 2);

        // 既然 snd_wnd 设为了 4096，我们的阈值必须 >= 4096。
        // 设置为 4096，允许 KCP 发送队列填满整个窗口，跑满带宽。
        const int CONGESTION_THRESHOLD = 4096;

        for (int i = 0; i < total_chunks; ++i) {
            file.read(buffer.data(), CHUNK_DATA_SIZE);
            std::streamsize bytes_read = file.gcount();

            // 压缩
            snappy::Compress(buffer.data(), bytes_read, &compressed_data);

            // 发送并获取积压量
            int pending_packets = send_chunk(i, compressed_data);

            // [背压逻辑] 如果积压过多，主动休眠，让 IO 线程有机会把数据发出去
            if (pending_packets > CONGESTION_THRESHOLD) {
                // 积压严重 -> 睡 10ms (稍微温和一点，避免睡太死)
                int sleep_ms = (pending_packets > CONGESTION_THRESHOLD * 1.5) ? 10 : 2;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
            // 如果 pending_packets 很小，说明网络通畅，循环全速运行，不 sleep
        }

        // 4. 结束处理
        if (!file_hash.empty()) {
            self->m_state_manager->record_file_sent(peer_id, requested_path_str, file_hash);
        }

        // 从 UI 列表中移除
        {
            std::lock_guard<std::mutex> lock(self->m_transfer_mutex);
            self->m_sending_files.erase(requested_path_str);
        }

        g_logger->info("[Transfer] 文件发送完成: {} (共 {} 块)", requested_path_str, total_chunks);
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