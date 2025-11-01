#include "VeritasSync/P2PManager.h"
#include "VeritasSync/Logger.h"

#include <snappy.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream> // 保留以防万一，但主要使用 Logger
#include <nlohmann/json.hpp>
#include <sstream>

#include "VeritasSync/Hashing.h"  // <-- 需要 Hashing
#include "VeritasSync/Protocol.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/SyncManager.h"

#define BUFFERSIZE 8192

#include <b64/decode.h>
#include <b64/encode.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

    static const uint8_t MSG_TYPE_JSON = 0x01;
    static const uint8_t MSG_TYPE_BINARY_CHUNK = 0x02;

    void append_uint16(std::string& s, uint16_t val) {
        uint16_t net_val =
            boost::asio::detail::socket_ops::host_to_network_short(val);
        s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
    }

    // 写入 4 字节的网络序 uint32_t
    void append_uint32(std::string& s, uint32_t val) {
        uint32_t net_val = boost::asio::detail::socket_ops::host_to_network_long(val);
        s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
    }

    // 读取 2 字节的网络序 uint16_t
    uint16_t read_uint16(const char*& data, size_t& len) {
        if (len < sizeof(uint16_t)) return 0;
        uint16_t net_val;
        std::memcpy(&net_val, data, sizeof(net_val));
        data += sizeof(net_val);
        len -= sizeof(net_val);
        return boost::asio::detail::socket_ops::network_to_host_short(net_val);
    }

    // 读取 4 字节的网络序 uint32_t
    uint32_t read_uint32(const char*& data, size_t& len) {
        if (len < sizeof(uint32_t)) return 0;
        uint32_t net_val;
        std::memcpy(&net_val, data, sizeof(net_val));
        data += sizeof(net_val);
        len -= sizeof(net_val);
        return boost::asio::detail::socket_ops::network_to_host_long(net_val);
    }

    //================================================================================
    // PeerContext 实现
    //================================================================================
    PeerContext::PeerContext(udp::endpoint ep,
        std::shared_ptr<P2PManager> manager_ptr)
        : endpoint(std::move(ep)), p2p_manager_ptr(std::move(manager_ptr)) {
    }

    PeerContext::~PeerContext() {
        if (kcp) {
            ikcp_release(kcp);
            kcp = nullptr;
        }
    }

    void PeerContext::setup_kcp(uint32_t conv) {
        kcp = ikcp_create(conv, this);
        kcp->output = &P2PManager::kcp_output_callback;
        ikcp_nodelay(kcp, 1, 10, 2, 1);
        ikcp_wndsize(kcp, 256, 256);
    }

    //================================================================================
    // P2PManager 实现
    //================================================================================

    void P2PManager::set_encryption_key(const std::string& key_string) {
        unsigned char hash[SHA256_DIGEST_LENGTH];

        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, key_string.c_str(), key_string.length());
        SHA256_Final(hash, &sha256);

        // m_encryption_key 现在存储的是 32 字节的原始 SHA-256 哈希值
        m_encryption_key.assign(reinterpret_cast<const char*>(hash),
            SHA256_DIGEST_LENGTH);

        g_logger->info("[P2P] 加密密钥已从 'sync_key' 派生。");
    }

    // 定义 GCM 所需的常量
    static const int GCM_IV_LEN = 12;   // 推荐的 12 字节 (96 位) IV
    static const int GCM_TAG_LEN = 16;  // 16 字节 (128 位) 认证标签

    // 辅助函数：加密
    std::string P2PManager::encrypt_gcm(const std::string& plaintext) {
        if (m_encryption_key.empty()) {
            g_logger->error("[KCP] 加密失败：密钥未设置。");
            return "";
        }

        // 1. 生成 12 字节的随机 IV (Initialization Vector)
        unsigned char iv[GCM_IV_LEN];
        if (RAND_bytes(iv, sizeof(iv)) != 1) {
            g_logger->error("[KCP] 加密失败：无法生成 IV。");
            return "";
        }

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        // 2. 初始化加密操作 (AES-256-GCM)
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
        // 3. 设置 IV 长度 (GCM 默认为 12)
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
        // 4. 设置密钥和 IV
        EVP_EncryptInit_ex(
            ctx, NULL, NULL,
            reinterpret_cast<const unsigned char*>(m_encryption_key.c_str()), iv);

        int out_len;
        std::vector<unsigned char> ciphertext(plaintext.length() +
            EVP_MAX_BLOCK_LENGTH);
        // 5. 加密数据
        EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len,
            reinterpret_cast<const unsigned char*>(plaintext.c_str()),
            plaintext.length());
        int ciphertext_len = out_len;

        // 6. 结束加密 (获取最后的密文块)
        EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &out_len);
        ciphertext_len += out_len;

        // 7. 获取 16 字节的认证标签
        unsigned char tag[GCM_TAG_LEN];
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag);

        EVP_CIPHER_CTX_free(ctx);

        // 8. 构造我们的数据包：[IV] + [Ciphertext] + [Tag]
        std::string final_payload;
        final_payload.append(reinterpret_cast<const char*>(iv), GCM_IV_LEN);
        final_payload.append(reinterpret_cast<const char*>(ciphertext.data()),
            ciphertext_len);
        final_payload.append(reinterpret_cast<const char*>(tag), GCM_TAG_LEN);

        return final_payload;
    }

    // 辅助函数：解密
    std::string P2PManager::decrypt_gcm(const std::string& ciphertext) {
        if (m_encryption_key.empty()) {
            g_logger->error("[KCP] 解密失败：密钥未设置。");
            return "";
        }

        if (ciphertext.length() < GCM_IV_LEN + GCM_TAG_LEN) {
            g_logger->warn("[KCP] 解密失败：数据包过短 ({} bytes)。", ciphertext.length());
            return "";
        }

        // 1. 解析数据包：[IV] + [Ciphertext] + [Tag]
        const unsigned char* iv =
            reinterpret_cast<const unsigned char*>(ciphertext.c_str());
        const unsigned char* tag = reinterpret_cast<const unsigned char*>(ciphertext.c_str() + ciphertext.length() - GCM_TAG_LEN);
        const unsigned char* encrypted_data = reinterpret_cast<const unsigned char*>(ciphertext.c_str() + GCM_IV_LEN);
        int encrypted_data_len = ciphertext.length() - GCM_IV_LEN - GCM_TAG_LEN;

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        // 2. 初始化解密
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
        // 3. 设置 IV 长度
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
        // 4. 设置密钥和 IV
        EVP_DecryptInit_ex(
            ctx, NULL, NULL,
            reinterpret_cast<const unsigned char*>(m_encryption_key.c_str()), iv);

        int out_len;
        std::vector<unsigned char> plaintext(encrypted_data_len);
        // 5. 解密数据
        EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, encrypted_data,
            encrypted_data_len);
        int plaintext_len = out_len;

        // 6. *关键*：在解密完成前，设置期望的认证标签
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN,
            const_cast<unsigned char*>(tag));

        // 7. 结束解密。
        // *如果标签不匹配* (数据被篡改或密钥错误)，这一步会失败 (返回 0)
        int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &out_len);

        EVP_CIPHER_CTX_free(ctx);

        if (ret > 0) {
            // 成功
            plaintext_len += out_len;
            return std::string(reinterpret_cast<const char*>(plaintext.data()),
                plaintext_len);
        } else {
            // 失败！(标签不匹配)
            g_logger->warn("[KCP] 解密失败：认证标签不匹配 (数据可能被篡改或密钥错误)。");
            return "";
        }
    }

    boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

    // --- 增量更新：全量广播 (用于启动) ---
    void P2PManager::broadcast_current_state() {
        if (m_role != SyncRole::Source) return;
        if (!m_state_manager) return;

        g_logger->info("[P2P] (Source) 文件系统发生变化，正在向所有节点广播最新状态...");

        m_state_manager->scan_directory();
        std::string json_state = m_state_manager->get_state_as_json_string();

        std::vector<udp::endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (const auto& pair : m_peers) {
                endpoints.push_back(pair.first);
            }
        }

        for (const auto& endpoint : endpoints) {
            send_over_kcp(json_state, endpoint);
        }
    }

    // --- 增量更新：广播单个文件更新 ---
    void P2PManager::broadcast_file_update(const FileInfo& file_info) {
        if (m_role != SyncRole::Source) return;

        g_logger->info("[P2P] (Source) 广播增量更新: {}", file_info.path);

        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
        msg[Protocol::MSG_PAYLOAD] = file_info;  // 利用 to_json 自动转换

        std::vector<udp::endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (const auto& pair : m_peers) {
                endpoints.push_back(pair.first);
            }
        }

        for (const auto& endpoint : endpoints) {
            send_over_kcp(msg.dump(), endpoint);
        }
    }

    // --- 增量更新：广播单个文件删除 ---
    void P2PManager::broadcast_file_delete(const std::string& relative_path) {
        if (m_role != SyncRole::Source) return;

        g_logger->info("[P2P] (Source) 广播增量删除: {}", relative_path);

        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE;
        msg[Protocol::MSG_PAYLOAD] = { {"path", relative_path} };

        std::vector<udp::endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (const auto& pair : m_peers) {
                endpoints.push_back(pair.first);
            }
        }

        for (const auto& endpoint : endpoints) {
            send_over_kcp(msg.dump(), endpoint);
        }
    }

    void P2PManager::broadcast_dir_create(const std::string& relative_path) {
        if (m_role != SyncRole::Source) return;
        g_logger->info("[P2P] (Source) 广播增量目录创建: {}", relative_path);
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_CREATE;
        msg[Protocol::MSG_PAYLOAD] = { {"path", relative_path} };
        std::vector<udp::endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (const auto& pair : m_peers) {
                endpoints.push_back(pair.first);
            }
        }
        for (const auto& endpoint : endpoints) {
            send_over_kcp(msg.dump(), endpoint);
        }
    }

    void P2PManager::broadcast_dir_delete(const std::string& relative_path) {
        if (m_role != SyncRole::Source) return;
        g_logger->info("[P2P] (Source) 广播增量目录删除: {}", relative_path);
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_DELETE;
        msg[Protocol::MSG_PAYLOAD] = { {"path", relative_path} };
        std::vector<udp::endpoint> endpoints;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (const auto& pair : m_peers) {
                endpoints.push_back(pair.first);
            }
        }
        for (const auto& endpoint : endpoints) {
            send_over_kcp(msg.dump(), endpoint);
        }
    }


    // --- 静态工厂与构造函数 ---
    std::shared_ptr<P2PManager> P2PManager::create(unsigned short port) {
        struct P2PManagerMaker : public P2PManager {
            P2PManagerMaker(unsigned short p) : P2PManager(p) {}
        };
        auto manager = std::make_shared<P2PManagerMaker>(port);
        manager->init();
        return manager;
    }

    P2PManager::P2PManager(unsigned short port)
        : m_socket(m_io_context, udp::endpoint(udp::v4(), port)),
        m_kcp_update_timer(m_io_context) {
    }

    void P2PManager::set_state_manager(StateManager* sm) { m_state_manager = sm; }

    void P2PManager::set_role(SyncRole role) { m_role = role; }

    void P2PManager::init() {
        m_thread = std::jthread([this]() {
            g_logger->info("[P2P] IO context 在后台线程运行...");
            auto work_guard = boost::asio::make_work_guard(m_io_context);
            m_io_context.run();
            });
        start_receive();
        schedule_kcp_update();
    }

    P2PManager::~P2PManager() {
        m_io_context.stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    // --- KCP 核心集成 ---
    int P2PManager::kcp_output_callback(const char* buf, int len, ikcpcb* kcp,
        void* user) {
        PeerContext* context = static_cast<PeerContext*>(user);
        if (context && context->p2p_manager_ptr) {
            context->p2p_manager_ptr->raw_udp_send(buf, len, context->endpoint);
        }
        return 0;
    }

    void P2PManager::schedule_kcp_update() {
        m_kcp_update_timer.expires_after(std::chrono::milliseconds(10));
        m_kcp_update_timer.async_wait(
            [self = shared_from_this()](const boost::system::error_code& ec) {
                if (!ec) {
                    self->update_all_kcps();
                }
            });
    }

    // --- 回退：移除解密 ---
    void P2PManager::update_all_kcps() {
        auto current_time_ms =
            (IUINT32)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
            .count();

        std::vector<std::pair<std::string, udp::endpoint>> received_messages;

        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto const& [endpoint, context] : m_peers) {
                ikcp_update(context->kcp, current_time_ms);

                char buffer[MAX_UDP_PAYLOAD];
                int size;
                while ((size = ikcp_recv(context->kcp, buffer, sizeof(buffer))) > 0) {
                    // 直接存入明文
                    received_messages.emplace_back(std::string(buffer, size), endpoint);
                }
            }
        }

        for (const auto& msg_pair : received_messages) {
            handle_kcp_message(msg_pair.first, msg_pair.second);
        }

        schedule_kcp_update();
    }

    // --- 原始网络 I/O ---
    void P2PManager::raw_udp_send(const char* data, size_t len,
        const udp::endpoint& endpoint) {
        m_socket.async_send_to(boost::asio::buffer(data, len), endpoint,
            [](const boost::system::error_code&, std::size_t) {});
    }

    void P2PManager::connect_to_peers(
        const std::vector<std::string>& peer_addresses) {
        udp::resolver resolver(m_io_context);
        for (const auto& addr_str : peer_addresses) {
            size_t colon_pos = addr_str.find(':');
            if (colon_pos == std::string::npos) continue;

            std::string host = addr_str.substr(0, colon_pos);
            std::string port_str = addr_str.substr(colon_pos + 1);

            boost::system::error_code ec;
            udp::resolver::results_type endpoints =
                resolver.resolve(host, port_str, ec);
            if (!ec && !endpoints.empty()) {
                udp::endpoint target_endpoint = *endpoints.begin();
                g_logger->info("[P2P] 正在向 {} 发送PING以进行握手。", target_endpoint.address().to_string() + ":" + std::to_string(target_endpoint.port()));
                get_or_create_peer_context(target_endpoint);
                raw_udp_send("PING", 4, target_endpoint);
            }
        }
    }

    // --- 核心网络接收循环 ---
    void P2PManager::start_receive() {
        auto remote_endpoint = std::make_shared<udp::endpoint>();
        auto recv_buffer = std::make_shared<std::array<char, MAX_UDP_PAYLOAD>>();

        m_socket.async_receive_from(
            boost::asio::buffer(*recv_buffer), *remote_endpoint,
            [self = shared_from_this(), remote_endpoint, recv_buffer](
                const boost::system::error_code& error, std::size_t bytes) {
                    self->handle_receive(error, bytes, remote_endpoint, recv_buffer);
            });
    }

    void P2PManager::handle_receive(
        const boost::system::error_code& error, std::size_t bytes_transferred,
        std::shared_ptr<udp::endpoint> remote_endpoint,
        std::shared_ptr<std::array<char, MAX_UDP_PAYLOAD>> recv_buffer) {
        if (!error && bytes_transferred > 0) {
            if (bytes_transferred == 4 &&
                std::string(recv_buffer->data(), 4) == "PING") {
                g_logger->info("[P2P] 收到来自 {} 的 PING 握手请求。", remote_endpoint->address().to_string() + ":" + std::to_string(remote_endpoint->port()));
                auto peer_context = get_or_create_peer_context(*remote_endpoint);

                if (m_role == SyncRole::Source) {
                    g_logger->info("[KCP] (Source) 对方已准备就绪，通过KCP发送我们的文件状态...");
                    m_state_manager->scan_directory();
                    std::string json_state = m_state_manager->get_state_as_json_string();
                    send_over_kcp(json_state, *remote_endpoint);
                } else {
                    g_logger->info("[KCP] (Destination) 已收到 PING，等待 Source 状态。");
                }
            } else {
                auto peer_context = get_or_create_peer_context(*remote_endpoint);
                ikcp_input(peer_context->kcp, recv_buffer->data(), bytes_transferred);
            }
        }
        start_receive();
    }

    // --- 对等点管理 ---
    std::shared_ptr<PeerContext> P2PManager::get_or_create_peer_context(
        const udp::endpoint& endpoint) {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers.find(endpoint);
        if (it != m_peers.end()) {
            return it->second;
        }

        g_logger->info("[KCP] 检测到新的对等点，为其创建KCP上下文: {}", endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
        auto new_context =
            std::make_shared<PeerContext>(endpoint, shared_from_this());
        uint32_t conv = 12345;
        new_context->setup_kcp(conv);
        m_peers[endpoint] = new_context;
        return new_context;
    }

    // --- 上层应用消息处理 ---

    // --- 回退：移除加密 ---
    void P2PManager::send_over_kcp(const std::string& msg,
        const udp::endpoint& target_endpoint) {
        std::string json_packet;
        json_packet.push_back(MSG_TYPE_JSON);
        json_packet.append(msg);

        std::string encrypted_msg = encrypt_gcm(json_packet);
        if (encrypted_msg.empty()) {
            g_logger->error("[KCP] 错误：加密失败，JSON 消息未发送至 {}", target_endpoint.address().to_string() + ":" + std::to_string(target_endpoint.port()));
            return;
        }
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers.find(target_endpoint);
        if (it != m_peers.end()) {
            // 发送密文
            ikcp_send(it->second->kcp, encrypted_msg.c_str(), encrypted_msg.length());
        } else {
            g_logger->error("[KCP] 错误: 尝试向一个未建立KCP上下文的对等点发送消息: {}", target_endpoint.address().to_string() + ":" + std::to_string(target_endpoint.port()));
        }
    }
    void P2PManager::handle_kcp_message(const std::string& msg,
        const udp::endpoint& from_endpoint) {
        std::string decrypted_msg = decrypt_gcm(msg);
        if (decrypted_msg.empty()) {
            return;  // 认证失败或密钥错误，错误已在 decrypt_gcm 中打印
        }

        if (decrypted_msg.empty()) {
            g_logger->warn("[KCP] 收到空解密包。");
            return;
        }

        // --- 修改：解析包类型 ---
        uint8_t msg_type = decrypted_msg[0];
        // 消息的剩余部分是 payload
        std::string payload(decrypted_msg.begin() + 1, decrypted_msg.end());

        if (msg_type == MSG_TYPE_JSON) {
            // --- 这是原有的 JSON 处理逻辑 ---
            try {
                auto json = nlohmann::json::parse(payload);  // 解析 payload
                const std::string json_msg_type =
                    json.at(Protocol::MSG_TYPE).get<std::string>();
                auto& json_payload = json.at(Protocol::MSG_PAYLOAD);

                if (json_msg_type == Protocol::TYPE_SHARE_STATE &&
                    m_role == SyncRole::Destination) {
                    handle_share_state(json_payload, from_endpoint);
                } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE &&
                    m_role == SyncRole::Destination) {
                    handle_file_update(json_payload, from_endpoint);
                } else if (json_msg_type == Protocol::TYPE_FILE_DELETE &&
                    m_role == SyncRole::Destination) {
                    handle_file_delete(json_payload, from_endpoint);
                } else if (json_msg_type == Protocol::TYPE_REQUEST_FILE &&
                    m_role == SyncRole::Source) {
                    handle_file_request(json_payload, from_endpoint);
                } else if (json_msg_type == Protocol::TYPE_DIR_CREATE &&
                    m_role == SyncRole::Destination) {
                    handle_dir_create(json_payload);
                } else if (json_msg_type == Protocol::TYPE_DIR_DELETE &&
                    m_role == SyncRole::Destination) {
                    handle_dir_delete(json_payload, from_endpoint);
                }
                // *** 注意：TYPE_FILE_CHUNK 的 case 已被移除 ***

            }
            catch (const std::exception& e) {
                g_logger->error("[P2P] 处理KCP JSON消息时发生错误: {}", e.what());
                g_logger->error("       原始JSON: {}", payload);
            }
            // --- JSON 逻辑结束 ---

        } else if (msg_type == MSG_TYPE_BINARY_CHUNK) {
            // --- 新增：调用二进制块处理器 ---
            if (m_role == SyncRole::Destination) {
                handle_file_chunk(payload);  // 传递原始二进制 payload
            }
        } else {
            g_logger->error("[KCP] 收到未知消息类型: {}", (int)msg_type);
        }
    }

    // --- 增量更新：处理全量状态 ---
    void P2PManager::handle_share_state(const nlohmann::json& payload,
        const udp::endpoint& from_endpoint) {
        if (m_role != SyncRole::Destination) return;

        g_logger->info("[KCP] (Destination) 收到来自 {} (Source) 的 'share_state' 消息。", from_endpoint.address().to_string() + ":" + std::to_string(from_endpoint.port()));

        std::vector<FileInfo> remote_files =
            payload.at("files").get<std::vector<FileInfo>>();
        std::set<std::string> remote_dirs =
            payload.at("directories").get<std::set<std::string>>();

        m_state_manager->scan_directory();
        nlohmann::json temp_json =
            nlohmann::json::parse(m_state_manager->get_state_as_json_string());
        std::vector<FileInfo> local_files = temp_json.at(Protocol::MSG_PAYLOAD)
            .at("files")
            .get<std::vector<FileInfo>>();

        std::set<std::string> local_dirs = m_state_manager->get_local_directories();
        g_logger->info("[SyncManager] 正在比较本地目录 ({} 个) 与远程目录 ({} 个).", local_dirs.size(), remote_dirs.size());

        SyncActions file_actions =
            SyncManager::compare_states_and_get_requests(local_files, remote_files);
        // --- 新增：比较目录 ---
        DirSyncActions dir_actions =
            SyncManager::compare_dir_states(local_dirs, remote_dirs);

        if (!file_actions.files_to_delete.empty()) {
            g_logger->info("[Sync] 计划删除 {} 个本地多余的文件。", file_actions.files_to_delete.size());
            for (const auto& file_path_str : file_actions.files_to_delete) {
                std::filesystem::path relative_path(std::u8string_view(
                    reinterpret_cast<const char8_t*>(file_path_str.c_str()),
                    file_path_str.length()));
                std::filesystem::path full_path =
                    m_state_manager->get_root_path() / relative_path;

                std::error_code ec;
                if (std::filesystem::remove(full_path, ec)) {
                    // --- 修复：使用 UTF-8 的相对路径 ---
                    g_logger->info("[Sync] -> 已删除 (相对路径): {}", file_path_str);
                } else if (ec != std::errc::no_such_file_or_directory) {
                    // --- 修复：同样修改错误日志 ---
                    g_logger->error("[Sync] -> 删除失败 (相对路径): {} Error: {}", file_path_str, ec.message());
                }
            }
        }
        if (!dir_actions.dirs_to_delete.empty()) {
            g_logger->info("[Sync] 计划删除 {} 个本地多余的目录。", dir_actions.dirs_to_delete.size());
            for (const auto& dir_path_str : dir_actions.dirs_to_delete) {
                std::filesystem::path relative_path(std::u8string_view(
                    reinterpret_cast<const char8_t*>(dir_path_str.c_str()),
                    dir_path_str.length()));
                std::filesystem::path full_path =
                    m_state_manager->get_root_path() / relative_path;

                std::error_code ec;
                // --- 关键：使用 remove_all ---
                std::filesystem::remove_all(full_path, ec);
                if (!ec) {
                    g_logger->info("[Sync] -> 已删除目录 (相对路径): {}", dir_path_str);
                } else if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] -> 删除目录失败 (相对路径): {} Error: {}", dir_path_str, ec.message());
                }
            }
        }

        if (!dir_actions.dirs_to_create.empty()) {
            g_logger->info("[Sync] 计划创建 {} 个缺失的目录。", dir_actions.dirs_to_create.size());
            for (const auto& dir_path_str : dir_actions.dirs_to_create) {
                std::filesystem::path relative_path(std::u8string_view(
                    reinterpret_cast<const char8_t*>(dir_path_str.c_str()),
                    dir_path_str.length()));
                std::filesystem::path full_path =
                    m_state_manager->get_root_path() / relative_path;

                std::error_code ec;
                std::filesystem::create_directories(full_path, ec);
                if (!ec) {
                    g_logger->info("[Sync] -> 已创建目录 (相对路径): {}", dir_path_str);
                } else {
                    g_logger->error("[Sync] -> 创建目录失败 (相对路径): {} Error: {}", dir_path_str, ec.message());
                }
            }
        }

        if (!file_actions.files_to_request.empty()) {
            g_logger->info("[KCP] 计划向 {} (Source) 请求 {} 个缺失/过期的文件。", from_endpoint.address().to_string() + ":" + std::to_string(from_endpoint.port()), file_actions.files_to_request.size());
            for (const auto& file_path : file_actions.files_to_request) {
                nlohmann::json request_msg;
                request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                request_msg[Protocol::MSG_PAYLOAD] = { {"path", file_path} };
                send_over_kcp(request_msg.dump(), from_endpoint);
            }
        }
    }

    // --- 增量更新：处理单个文件更新 ---
    void P2PManager::handle_file_update(const nlohmann::json& payload,
        const udp::endpoint& from_endpoint) {
        if (m_role != SyncRole::Destination) return;

        FileInfo remote_info;
        try {
            remote_info = payload.get<FileInfo>();  // 利用 from_json 自动转换
        }
        catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 file_update 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量更新: {}", remote_info.path);

        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(remote_info.path.c_str()),
            remote_info.path.length()));
        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        std::error_code ec;
        bool should_request = false;

        if (!std::filesystem::exists(full_path, ec) || ec) {
            g_logger->info("[Sync] -> 本地不存在, 需要请求。");
            should_request = true;
        } else {
            std::string local_hash = Hashing::CalculateSHA256(full_path);
            if (local_hash != remote_info.hash) {
                g_logger->info("[Sync] -> 哈希不匹配 (本地: {} vs 远程: {}), 需要请求。", local_hash.substr(0, 7), remote_info.hash.substr(0, 7));
                should_request = true;
            } else {
                g_logger->info("[Sync] -> 哈希匹配, 已是最新。");
            }
        }

        if (should_request) {
            nlohmann::json request_msg;
            request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
            request_msg[Protocol::MSG_PAYLOAD] = { {"path", remote_info.path} };
            send_over_kcp(request_msg.dump(), from_endpoint);
        }
    }

    // --- 增量更新：处理单个文件删除 ---
    void P2PManager::handle_file_delete(const nlohmann::json& payload,
        const udp::endpoint& from_endpoint) {
        if (m_role != SyncRole::Destination) return;

        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        }
        catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 file_delete 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量删除: {}", relative_path_str);

        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(relative_path_str.c_str()),
            relative_path_str.length()));

        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        std::error_code ec;
        if (std::filesystem::remove(full_path, ec)) {
            g_logger->info("[Sync] -> 已删除本地文件 (相对路径): {}", relative_path_str);
            m_state_manager->remove_path_from_map(relative_path_str);
        } else {
            // 修复：使用 ec != ... 来正确比较
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] -> 删除本地文件失败 (相对路径): {} Error: {}", relative_path_str, ec.message());
            } else {
                g_logger->info("[Sync] -> 本地文件已不存在, 无需操作。");
            }
        }
    }

    void P2PManager::handle_dir_create(const nlohmann::json& payload) {
        if (m_role != SyncRole::Destination) return;

        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        }
        catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 dir_create 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录创建: {}", relative_path_str);

        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(relative_path_str.c_str()),
            relative_path_str.length()));
        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        std::error_code ec;
        if (std::filesystem::create_directories(full_path, ec)) {
            g_logger->info("[Sync] -> 已创建目录: {}", relative_path_str);
            m_state_manager->add_dir_to_map(relative_path_str);
        } else if (ec) {
            g_logger->error("[Sync] -> 创建目录失败: {} Error: {}", relative_path_str, ec.message());
        }
    }

    void P2PManager::handle_dir_delete(const nlohmann::json& payload,
        const udp::endpoint& from_endpoint) {
        if (m_role != SyncRole::Destination) return;

        std::string relative_path_str;
        try {
            relative_path_str = payload.at("path").get<std::string>();
        }
        catch (const std::exception& e) {
            g_logger->error("[KCP] (Destination) 解析 dir_delete 失败: {}", e.what());
            return;
        }

        g_logger->info("[KCP] (Destination) 收到增量目录删除: {}", relative_path_str);

        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(relative_path_str.c_str()),
            relative_path_str.length()));
        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        std::error_code ec;
        // 关键：使用 remove_all 来删除非空目录
        std::filesystem::remove_all(full_path, ec);

        if (!ec) {
            g_logger->info("[Sync] -> 已删除目录 (相对路径): {}", relative_path_str);
            m_state_manager->remove_dir_from_map(relative_path_str);
        } else {
            if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] -> 删除目录失败 (相对路径): {} Error: {}", relative_path_str, ec.message());
            } else {
                g_logger->info("[Sync] -> 本地目录已不存在, 无需操作。");
            }
        }
    }


    void P2PManager::handle_file_request(const nlohmann::json& payload,
        const udp::endpoint& from_endpoint) {
        const std::string requested_path_str = payload.at("path").get<std::string>();
        g_logger->info("[KCP] 收到来自 {} 对文件 '{}' 的请求。", from_endpoint.address().to_string() + ":" + std::to_string(from_endpoint.port()), requested_path_str);

        std::filesystem::path relative_path(std::u8string_view(
            reinterpret_cast<const char8_t*>(requested_path_str.c_str()),
            requested_path_str.length()));
        std::filesystem::path full_path =
            m_state_manager->get_root_path() / relative_path;

        if (!std::filesystem::exists(full_path)) {
            g_logger->error("[P2P] 被请求的文件不存在: {}", full_path.string());
            return;
        }

        std::ifstream file(full_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            g_logger->error("[P2P] 无法打开文件: {}", full_path.string());
            return;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        // (辅助 lambda send_binary_packet 保持不变)
        auto send_binary_packet = [&](std::string packet_payload) {
            std::string binary_packet;
            binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
            binary_packet.append(std::move(packet_payload));
            std::string encrypted_msg = encrypt_gcm(binary_packet);
            if (encrypted_msg.empty()) {
                g_logger->error("[KCP] 错误：加密失败，文件块未发送至 {}", from_endpoint.address().to_string() + ":" + std::to_string(from_endpoint.port()));
                return;
            }
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            auto it = m_peers.find(from_endpoint);
            if (it != m_peers.end()) {
                ikcp_send(it->second->kcp, encrypted_msg.c_str(), encrypted_msg.length());
            }
        };

        if (size == 0) {
            g_logger->info("[KCP] 正在发送零字节文件 '{}' 的元信息...", requested_path_str);
            std::string packet_payload;
            append_uint16(packet_payload,
                static_cast<uint16_t>(requested_path_str.length()));
            packet_payload.append(requested_path_str);
            append_uint32(packet_payload, 0);  // chunk_index
            append_uint32(packet_payload, 1);  // total_chunks
            // (数据部分为空)
            send_binary_packet(std::move(packet_payload));
            return;
        }

        int total_chunks =
            static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
        std::vector<char> buffer(CHUNK_DATA_SIZE);

        g_logger->info("[KCP] 正在将文件 '{}' ({} 字节) 分成 {} 块 (压缩并) 发送给 {}", 
            requested_path_str, size, total_chunks, from_endpoint.address().to_string() + ":" + std::to_string(from_endpoint.port()));

        for (int i = 0; i < total_chunks; ++i) {
            file.read(buffer.data(), CHUNK_DATA_SIZE);
            std::streamsize bytes_read = file.gcount();

            // --- 核心修改：压缩 ---
            std::string compressed_data;
            snappy::Compress(buffer.data(), bytes_read, &compressed_data);
            // ---

            std::string packet_payload;
            append_uint16(packet_payload,
                static_cast<uint16_t>(requested_path_str.length()));
            packet_payload.append(requested_path_str);
            append_uint32(packet_payload, i);
            append_uint32(packet_payload, total_chunks);
            // --- 核心修改：附加压缩后的数据 ---
            packet_payload.append(compressed_data);

            send_binary_packet(std::move(packet_payload));
        }
    }
    void P2PManager::handle_file_chunk(const std::string& payload) {
        const char* data_ptr = payload.c_str();
        size_t data_len = payload.length();

        // 1. 读取路径
        uint16_t path_len = read_uint16(data_ptr, data_len);
        if (path_len == 0 || data_len < path_len) {
            g_logger->error("[KCP] 二进制块解析失败：路径长度无效。");
            return;
        }
        std::string file_path_str(data_ptr, path_len);
        data_ptr += path_len;
        data_len -= path_len;

        // 2. 读取索引和总数
        uint32_t chunk_index = read_uint32(data_ptr, data_len);
        uint32_t total_chunks = read_uint32(data_ptr, data_len);

        // 3. 剩余的是 *压缩* 数据
        std::string compressed_chunk_data(data_ptr, data_len);

        // --- 核心修改：解压缩 ---
        std::string uncompressed_data;
        if (data_len == 0) {
            // 数据为空，解压后的数据自然也为空。
            // (uncompressed_data 已经是空字符串，我们什么都不用做)
        } else if (!snappy::Uncompress(compressed_chunk_data.data(),
            compressed_chunk_data.size(),
            &uncompressed_data)) {
            g_logger->error("[KCP] Snappy 解压失败 (包可能已损坏): {}", file_path_str);
            return;
        }
        // ---

        auto& assembly_info = m_file_assembly_buffer[file_path_str];
        assembly_info.first = total_chunks;
        assembly_info.second[chunk_index] =
            std::move(uncompressed_data);  // 存储解压后的数据

        // (美化日志输出)
        g_logger->debug("[KCP] 收到文件 '{}' 的块 {}/{} (压缩后: {} 字节, 解压后: {} 字节).",
            file_path_str, chunk_index + 1, total_chunks,
            compressed_chunk_data.size(), assembly_info.second[chunk_index].size());

        // (文件重组逻辑保持不变)
        if (assembly_info.second.size() == total_chunks) {
            g_logger->info("[KCP] 文件 '{}' 的所有块已收齐，正在重组...", file_path_str);

            std::filesystem::path relative_path(std::u8string_view(
                reinterpret_cast<const char8_t*>(file_path_str.c_str()),
                file_path_str.length()));

            std::filesystem::path full_path =
                m_state_manager->get_root_path() / relative_path;

            if (full_path.has_parent_path()) {
                std::filesystem::create_directories(full_path.parent_path());
            }

            std::ofstream output_file(full_path, std::ios::binary);
            if (!output_file.is_open()) {
                g_logger->error("[P2P] 创建文件失败: {}", full_path.string());
                m_file_assembly_buffer.erase(file_path_str);
                return;
            }

            for (int i = 0; i < total_chunks; ++i) {
                output_file.write(assembly_info.second[i].data(),
                    assembly_info.second[i].length());
            }
            output_file.close();

            g_logger->info("[P2P] 成功: 文件 '{}' 已保存。", file_path_str);
            m_file_assembly_buffer.erase(file_path_str);
        }
    }

}  // namespace VeritasSync
