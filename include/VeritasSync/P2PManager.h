#pragma once

#include <ikcp.h>

#include <array>
#include <boost/asio.hpp>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "VeritasSync/Protocol.h"  // 需要包含 Protocol.h

namespace VeritasSync {

    // --- 定义同步角色 ---
    enum class SyncRole { Source, Destination };

    class StateManager;
    class P2PManager;

    using boost::asio::ip::udp;

    // (PeerContext 结构体不变)
    struct PeerContext {
        udp::endpoint endpoint;
        ikcpcb* kcp = nullptr;
        std::shared_ptr<P2PManager> p2p_manager_ptr;
        PeerContext(udp::endpoint ep, std::shared_ptr<P2PManager> manager_ptr);
        ~PeerContext();
        void setup_kcp(uint32_t conv);
    };

    class StateManager;

    class P2PManager : public std::enable_shared_from_this<P2PManager> {
    public:
        boost::asio::io_context& get_io_context();
        static std::shared_ptr<P2PManager> create(unsigned short port);

        void set_encryption_key(const std::string& key_string);

        // --- 依赖注入 ---
        void set_state_manager(StateManager* sm);
        void set_role(SyncRole role);

        static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp,
            void* user);

        ~P2PManager();
        void connect_to_peers(const std::vector<std::string>& peer_addresses);

        void raw_udp_send(const char* data, size_t len,
            const udp::endpoint& endpoint);

        // --- 广播方法 (由 StateManager 调用) ---
        void broadcast_current_state();  // 用于启动时的全量同步
        void broadcast_file_update(const FileInfo& file_info);
        void broadcast_file_delete(const std::string& relative_path);
        void broadcast_dir_create(const std::string& relative_path);
        void broadcast_dir_delete(const std::string& relative_path);

    private:
        static constexpr size_t MAX_UDP_PAYLOAD = 16384;
        static constexpr size_t CHUNK_DATA_SIZE = 8192;

        P2PManager(unsigned short port);
        void init();

        // --- 核心网络循环 ---
        void start_receive();
        void handle_receive(
            const boost::system::error_code& error, std::size_t bytes_transferred,
            std::shared_ptr<udp::endpoint> remote_endpoint,
            std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer);

        // --- KCP 集成 ---
        void schedule_kcp_update();
        void update_all_kcps();
        std::shared_ptr<PeerContext> get_or_create_peer_context(
            const udp::endpoint& endpoint);

        // --- 上层应用逻辑 (由KCP调用) ---
        void send_over_kcp(const std::string& msg,
            const udp::endpoint& target_endpoint);
        void handle_kcp_message(const std::string& msg,
            const udp::endpoint& from_endpoint);

        // --- 具体的消息处理器 ---
        void handle_share_state(const nlohmann::json& payload, const udp::endpoint& from_endpoint);
        void handle_file_update(const nlohmann::json& payload, const udp::endpoint& from_endpoint);
        void handle_file_delete(const nlohmann::json& payload, const udp::endpoint& from_endpoint);
        void handle_file_request(const nlohmann::json& payload, const udp::endpoint& from_endpoint);
        void handle_file_chunk(const std::string& payload);
        void handle_dir_create(const nlohmann::json& payload);
        void handle_dir_delete(const nlohmann::json& payload, const udp::endpoint& from_endpoint);

        std::string encrypt_gcm(const std::string& plaintext);
        std::string decrypt_gcm(const std::string& ciphertext);

        // --- 成员变量 ---
        boost::asio::io_context m_io_context;
        udp::socket m_socket;
        std::jthread m_thread;
        boost::asio::steady_timer m_kcp_update_timer;

        StateManager* m_state_manager = nullptr;
        SyncRole m_role = SyncRole::Source;

        std::map<udp::endpoint, std::shared_ptr<PeerContext>> m_peers;
        std::mutex m_peers_mutex;

        std::map<std::string, std::pair<int, std::map<int, std::string>>>
            m_file_assembly_buffer;
        std::string m_encryption_key;  // 存储 32 字节的 SHA-256 派生密钥
    };

}  // namespace VeritasSync
