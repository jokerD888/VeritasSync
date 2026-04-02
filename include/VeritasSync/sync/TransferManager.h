#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
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
    bool is_stalled = false;
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

    /// 从 Config::Transfer 注入的运行时参数
    struct TransferConfig {
        int file_open_max_retries       = 5;
        int file_open_retry_delay_ms    = 200;
        int stall_threshold_ms          = 5000;
        int zombie_threshold_seconds    = 10;
        int receive_timeout_minutes     = 10;
        int congestion_wait_high_ms     = 200;
        int congestion_wait_low_ms      = 100;
        int congestion_high_multiplier  = 4;
        int congestion_threshold        = 256;
        double speed_update_interval_sec = 0.5;
        size_t max_total_chunks         = 8388608;
        size_t max_path_length          = 4096;
    };

    // 构造函数注入所有依赖
    TransferManager(StateManager* sm, boost::asio::io_context& io_context,
                    boost::asio::thread_pool& pool, SendCallback send_cb,
                    size_t chunk_size = DEFAULT_CHUNK_DATA_SIZE,
                    TransferConfig config = {});

    void set_state_manager(StateManager* sm) { m_state_manager = sm; }

    // [Source端] 处理文件请求 (异步读盘 -> 压缩 -> 加密 -> 回调发送)
    void queue_upload(const std::string& peer_id, const nlohmann::json& request_payload);

    // [Destination端] 处理接收到的二进制文件块 (解密在 P2PManager 完成，这里处理：解压 -> 写入 -> 重命名)
    // 注意：为了性能，传入 payload 应该是已经解密后的明文数据（包含头部信息的二进制块）
    void handle_chunk(std::string decrypted_payload, const std::string& peer_id);

    // 获取当前活跃的下载任务 (WebUI 用)
    std::vector<TransferStatus> get_active_transfers();

    // 清理超时的接收任务 (需外部定时调用)
    void cleanup_stale_buffers();

    SessionStats get_session_stats() const;

    // 【修复 #7】默认 chunk 大小（静态常量，向后兼容）
    static constexpr size_t DEFAULT_CHUNK_DATA_SIZE = 16384;
    // 运行时实际使用的 chunk 大小（从配置读取）
    const size_t CHUNK_DATA_SIZE;
    
    // ====== 断点续传相关 ======
    
    /**
     * @brief 续传信息结构体
     */
    struct ResumeInfo {
        std::string path;
        uint32_t received_chunks;
        uint32_t total_chunks;
        std::string expected_hash;
        uint64_t expected_size;
        std::string temp_path;
    };
    
    /**
     * @brief 清理指定 peer 的所有接收任务
     * 
     * 当对端程序正常关闭（收到 goodbye）时调用，
     * 清理该 peer 的所有未完成传输状态和临时文件。
     * 
     * @param peer_id 对端 ID
     */
    void cancel_receives_for_peer(const std::string& peer_id);
    
    /**
     * @brief 检查是否可以续传指定文件
     * 
     * 检查内存中是否有未完成的传输任务，并校验源文件是否变化。
     * 
     * @param path 文件路径
     * @param remote_hash 远程文件的当前 hash
     * @param remote_size 远程文件的当前大小
     * @return 如果可以续传返回 ResumeInfo，否则返回 nullopt
     */
    std::optional<ResumeInfo> check_resume_eligibility(
        const std::string& path,
        const std::string& remote_hash,
        uint64_t remote_size);
    
    /**
     * @brief 预注册接收任务的元数据
     * 
     * 在发送 request_file 之前调用，记录预期的文件信息。
     * 
     * @param path 文件路径
     * @param peer_id 来源 peer ID
     * @param hash 预期的文件 hash
     * @param size 预期的文件大小
     */
    void register_expected_metadata(
        const std::string& path,
        const std::string& peer_id,
        const std::string& hash,
        uint64_t size);

    /**
     * @brief 获取指定 peer 的未完成接收任务路径列表
     *
     * 用于连接恢复后的诊断日志，了解哪些文件可能需要重传。
     *
     * @param peer_id 对端 ID
     * @return 未完成接收的文件路径列表
     */
    std::vector<std::string> get_pending_receives_for_peer(const std::string& peer_id) const;

private:
    StateManager* m_state_manager;
    boost::asio::io_context& m_io_context;
    boost::asio::thread_pool& m_worker_pool;
    SendCallback m_send_callback;
    TransferConfig m_transfer_config;  // 运行时配置参数

    struct ReceivingFile {
        // C-2: per-file 锁 —— 保护 file_stream 和写入操作
        // 允许不同文件的 chunk 并行处理，同一文件的 chunk 串行写入
        std::mutex file_mutex;
        
        std::ofstream file_stream;
        std::string temp_path;
        uint32_t total_chunks = 0;
        // 【修复】使用 bitset 替代简单计数器，正确处理 KCP 重传导致的重复 chunk
        // received_chunks 仍然用于快速计数（只在新 chunk 时递增）
        uint32_t received_chunks = 0;
        std::vector<bool> received_bitmap;  // 标记每个 chunk 是否已接收
        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();

        uint32_t last_tick_chunks = 0;
        std::chrono::steady_clock::time_point last_tick_time = std::chrono::steady_clock::now();
        double current_speed = 0.0;
        
        // --- 断点续传相关字段 ---
        std::string peer_id;           // 来源 peer ID（用于按 peer 清理）
        std::string expected_hash;     // 预期文件哈希（用于校验源文件未变）
        uint64_t expected_size = 0;    // 预期文件大小
        
        // C-2: 标记是否正在被 handle_chunk 使用（防止 cancel/cleanup 并发删除）
        // 【安全修复 H5】改为 atomic，消除 worker 线程与 io_context 线程的数据竞争
        std::atomic<bool> busy{false};
    };
    struct SendingFile {
        uint32_t total_chunks = 0;
        uint32_t sent_chunks = 0;

        uint32_t last_tick_chunks = 0;
        std::chrono::steady_clock::time_point last_tick_time = std::chrono::steady_clock::now();
        double current_speed = 0.0;
        // 记录发送活跃时间
        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
    };

    // C-2 超长函数拆分：handle_chunk 的子步骤

    /// handle_chunk 解析二进制头部并解压数据
    struct ChunkHeader {
        std::string file_path;
        uint32_t chunk_index = 0;
        uint32_t total_chunks = 0;
        std::string uncompressed_data;
        bool valid = false;
    };
    static ChunkHeader parse_chunk_payload(const std::string& payload);

    struct UploadRequest {
        std::string path;
        uint32_t start_chunk = 0;
        std::string expected_hash;
        uint64_t expected_size = 0;
    };

    static std::optional<UploadRequest> parse_and_validate_upload_request(
        const nlohmann::json& request_payload,
        StateManager* state_manager,
        std::string& error_reason);

    static bool validate_chunk_header(const ChunkHeader& hdr, std::string& error_reason);


    /// handle_chunk 查找/创建 ReceivingFile 条目（短暂全局锁内）
    struct ChunkLookupResult {
        std::shared_ptr<ReceivingFile> recv_ptr;
        bool need_open_stream = false;
        bool need_create_dirs = false;
        std::filesystem::path full_path;
        std::filesystem::path temp_path;
    };
    ChunkLookupResult lookup_or_create_receiving(const std::string& file_path_str,
                                                  uint32_t total_chunks,
                                                  const std::string& peer_id);

    /// handle_chunk 完成接收后的收尾（rename / hash / 记录）
    void finalize_received_file(const std::string& file_path_str,
                                std::shared_ptr<ReceivingFile>& recv_ptr,
                                const std::string& peer_id);

    // 正在接收的文件映射 (Path -> State)
    // C-2: 使用 shared_ptr 因为 ReceivingFile 含 std::mutex 不可移动
    std::unordered_map<std::string, std::shared_ptr<ReceivingFile>> m_receiving_files;
    mutable std::mutex m_transfer_mutex;  // 全局锁：保护 map 结构（insert/erase/遍历）
    // 【健壮性修复 H6】以 "peer_id\0path" 为 key，防止多 peer 同时请求同一文件时进度覆盖
    std::unordered_map<std::string, SendingFile> m_sending_files;  // 追踪上传
    static std::string make_sending_key(const std::string& peer_id, const std::string& path) {
        return peer_id + '\0' + path;
    }

    std::atomic<uint64_t> m_session_total{0};
    std::atomic<uint64_t> m_session_done{0};
};

}  // namespace VeritasSync