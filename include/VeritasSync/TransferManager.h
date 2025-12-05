#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace VeritasSync {

class StateManager;
class CryptoLayer;

// 用于 WebUI 显示的状态结构
struct TransferStatus {
    std::string path;
    uint32_t total_chunks;
    uint32_t processed_chunks;  // 已处理块数 (发送或接收)
    float progress;
    bool is_upload;  // true=上传, false=下载
    double speed;
};

// 定义发送回调：(目标PeerID, 加密后的数据)
// TransferManager 在 Worker 线程完成处理后，会调用此回调
using SendCallback = std::function<int(const std::string& peer_id, const std::string& encrypted_data)>;

class TransferManager : public std::enable_shared_from_this<TransferManager> {
public:
    struct SessionStats {
        uint64_t total;  // 发起的总任务数
        uint64_t done;   // 成功完成的任务数
    };

    // 构造函数注入所有依赖
    TransferManager(StateManager* sm, boost::asio::thread_pool& pool, CryptoLayer& crypto, SendCallback send_cb);

    void set_state_manager(StateManager* sm) { m_state_manager = sm; }

    // [Source端] 处理文件请求 (异步读盘 -> 压缩 -> 加密 -> 回调发送)
    void queue_upload(const std::string& peer_id, const nlohmann::json& request_payload);

    // [Destination端] 处理接收到的二进制文件块 (解密在 P2PManager 完成，这里处理：解压 -> 写入 -> 重命名)
    // 注意：为了性能，传入 payload 应该是已经解密后的明文数据（包含头部信息的二进制块）
    void handle_chunk(const std::string& decrypted_payload, const std::string& peer_id);

    // 获取当前活跃的下载任务 (WebUI 用)
    std::vector<TransferStatus> get_active_transfers();

    // 清理超时的接收任务 (需外部定时调用)
    void cleanup_stale_buffers();

    SessionStats get_session_stats() const;

    static constexpr size_t CHUNK_DATA_SIZE = 16384;

private:
    StateManager* m_state_manager;
    boost::asio::thread_pool& m_worker_pool;
    CryptoLayer& m_crypto;
    SendCallback m_send_callback;

    struct ReceivingFile {
        std::ofstream file_stream;
        std::string temp_path;
        uint32_t total_chunks = 0;
        uint32_t received_chunks = 0;
        std::chrono::steady_clock::time_point last_active;

        uint32_t last_tick_chunks = 0;
        std::chrono::steady_clock::time_point last_tick_time = std::chrono::steady_clock::now();
        double current_speed = 0.0;
    };
    struct SendingFile {
        uint32_t total_chunks = 0;
        uint32_t sent_chunks = 0;

        uint32_t last_tick_chunks = 0;
        std::chrono::steady_clock::time_point last_tick_time = std::chrono::steady_clock::now();
        double current_speed = 0.0;
    };

    // 正在接收的文件映射 (Path -> State)
    std::map<std::string, ReceivingFile> m_receiving_files;
    std::mutex m_transfer_mutex;
    std::map<std::string, SendingFile> m_sending_files;  // 追踪上传

    std::atomic<uint64_t> m_session_total{0};
    std::atomic<uint64_t> m_session_done{0};
};

}  // namespace VeritasSync