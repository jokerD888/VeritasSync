#include "VeritasSync/P2PManager.h"

#include <snappy.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "VeritasSync/Hashing.h"
#include "VeritasSync/Logger.h"
#include "VeritasSync/Protocol.h"
#include "VeritasSync/StateManager.h"
#include "VeritasSync/SyncManager.h"
#include "VeritasSync/TrackerClient.h"
#define BUFFERSIZE 8192
#include <b64/decode.h>
#include <b64/encode.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

// (append/read uint16/32 辅助函数 ... 保持不变 ...)
void append_uint16(std::string& s, uint16_t val) {
    uint16_t net_val = boost::asio::detail::socket_ops::host_to_network_short(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}
void append_uint32(std::string& s, uint32_t val) {
    uint32_t net_val = boost::asio::detail::socket_ops::host_to_network_long(val);
    s.append(reinterpret_cast<const char*>(&net_val), sizeof(net_val));
}
uint16_t read_uint16(const char*& data, size_t& len) {
    if (len < sizeof(uint16_t)) return 0;
    uint16_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_short(net_val);
}
uint32_t read_uint32(const char*& data, size_t& len) {
    if (len < sizeof(uint32_t)) return 0;
    uint32_t net_val;
    std::memcpy(&net_val, data, sizeof(net_val));
    data += sizeof(net_val);
    len -= sizeof(net_val);
    return boost::asio::detail::socket_ops::network_to_host_long(net_val);
}

static const uint8_t MSG_TYPE_JSON = 0x01;
static const uint8_t MSG_TYPE_BINARY_CHUNK = 0x02;

// (PeerContext 实现 ... 保持不变 ...)
PeerContext::PeerContext(std::string id, juice_agent_t* ag, std::shared_ptr<P2PManager> manager_ptr)
    : peer_id(std::move(id)), agent(ag), p2p_manager_ptr(std::move(manager_ptr)) {}
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

// (set_encryption_key, encrypt_gcm, decrypt_gcm ... 保持不变 ...)
void P2PManager::set_encryption_key(const std::string& key_string) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, key_string.c_str(), key_string.length());
    SHA256_Final(hash, &sha256);
    m_encryption_key.assign(reinterpret_cast<const char*>(hash), SHA256_DIGEST_LENGTH);
    g_logger->info("[P2P] 加密密钥已从 'sync_key' 派生。");
}
static const int GCM_IV_LEN = 12;
static const int GCM_TAG_LEN = 16;
std::string P2PManager::encrypt_gcm(const std::string& plaintext) {
    if (m_encryption_key.empty()) {
        g_logger->error("[KCP] 加密失败：密钥未设置。");
        return "";
    }
    unsigned char iv[GCM_IV_LEN];
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        g_logger->error("[KCP] 加密失败：无法生成 IV。");
        return "";
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
    EVP_EncryptInit_ex(ctx, NULL, NULL, reinterpret_cast<const unsigned char*>(m_encryption_key.c_str()), iv);
    int out_len;
    std::vector<unsigned char> ciphertext(plaintext.length() + EVP_MAX_BLOCK_LENGTH);
    EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len, reinterpret_cast<const unsigned char*>(plaintext.c_str()),
                      plaintext.length());
    int ciphertext_len = out_len;
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &out_len);
    ciphertext_len += out_len;
    unsigned char tag[GCM_TAG_LEN];
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag);
    EVP_CIPHER_CTX_free(ctx);
    std::string final_payload;
    final_payload.append(reinterpret_cast<const char*>(iv), GCM_IV_LEN);
    final_payload.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext_len);
    final_payload.append(reinterpret_cast<const char*>(tag), GCM_TAG_LEN);
    return final_payload;
}
std::string P2PManager::decrypt_gcm(const std::string& ciphertext) {
    if (m_encryption_key.empty()) {
        g_logger->error("[KCP] 解密失败：密钥未设置。");
        return "";
    }
    if (ciphertext.length() < GCM_IV_LEN + GCM_TAG_LEN) {
        g_logger->warn("[KCP] 解密失败：数据包过短 ({} bytes)。", ciphertext.length());
        return "";
    }
    const unsigned char* iv = reinterpret_cast<const unsigned char*>(ciphertext.c_str());
    const unsigned char* tag =
        reinterpret_cast<const unsigned char*>(ciphertext.c_str() + ciphertext.length() - GCM_TAG_LEN);
    const unsigned char* encrypted_data = reinterpret_cast<const unsigned char*>(ciphertext.c_str() + GCM_IV_LEN);
    int encrypted_data_len = ciphertext.length() - GCM_IV_LEN - GCM_TAG_LEN;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_IV_LEN, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, reinterpret_cast<const unsigned char*>(m_encryption_key.c_str()), iv);
    int out_len;
    std::vector<unsigned char> plaintext(encrypted_data_len);
    EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, encrypted_data, encrypted_data_len);
    int plaintext_len = out_len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, const_cast<unsigned char*>(tag));
    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &out_len);
    EVP_CIPHER_CTX_free(ctx);
    if (ret > 0) {
        plaintext_len += out_len;
        return std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext_len);
    } else {
        g_logger->warn("[KCP] 解密失败：认证标签不匹配 (数据可能被篡改或密钥错误)。");
        return "";
    }
}

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

// (广播方法 ... 保持不变 ...)
void P2PManager::broadcast_current_state() {
    if (m_role != SyncRole::Source) return;
    if (!m_state_manager) return;
    g_logger->info("[P2P] (Source) 文件系统发生变化，正在向所有节点广播最新状态...");
    m_state_manager->scan_directory();
    std::string json_state = m_state_manager->get_state_as_json_string();
    send_over_kcp(json_state);
}
void P2PManager::broadcast_file_update(const FileInfo& file_info) {
    if (m_role != SyncRole::Source) return;
    g_logger->info("[P2P] (Source) 广播增量更新: {}", file_info.path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
    msg[Protocol::MSG_PAYLOAD] = file_info;
    send_over_kcp(msg.dump());
}
void P2PManager::broadcast_file_delete(const std::string& relative_path) {
    if (m_role != SyncRole::Source) return;
    g_logger->info("[P2P] (Source) 广播增量删除: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_DELETE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}
void P2PManager::broadcast_dir_create(const std::string& relative_path) {
    if (m_role != SyncRole::Source) return;
    g_logger->info("[P2P] (Source) 广播增量目录创建: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_CREATE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}
void P2PManager::broadcast_dir_delete(const std::string& relative_path) {
    if (m_role != SyncRole::Source) return;
    g_logger->info("[P2P] (Source) 广播增量目录删除: {}", relative_path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_DELETE;
    msg[Protocol::MSG_PAYLOAD] = {{"path", relative_path}};
    send_over_kcp(msg.dump());
}

// --- 静态工厂与构造函数 (修复) ---
std::shared_ptr<P2PManager> P2PManager::create() {
    struct P2PManagerMaker : public P2PManager {
        P2PManagerMaker() : P2PManager() {}
    };
    auto manager = std::make_shared<P2PManagerMaker>();
    manager->init();
    return manager;
}

P2PManager::P2PManager() : m_io_context(), m_kcp_update_timer(m_io_context) {
    // (libjuice 日志设置 ... 保持不变 ...)
    juice_set_log_level(JUICE_LOG_LEVEL_DEBUG);
    juice_set_log_handler([](juice_log_level_t level, const char* message) {
        switch (level) {
        case JUICE_LOG_LEVEL_VERBOSE:
        case JUICE_LOG_LEVEL_DEBUG:
            g_logger->debug("[libjuice] {}", message);
            break;
        case JUICE_LOG_LEVEL_INFO:
            g_logger->info("[libjuice] {}", message);
            break;
        case JUICE_LOG_LEVEL_WARN:
            g_logger->warn("[libjuice] {}", message);
            break;
        case JUICE_LOG_LEVEL_ERROR:
        case JUICE_LOG_LEVEL_FATAL:
            g_logger->error("[libjuice] {}", message);
            break;
        default:
            break;
        }
    });
}

void P2PManager::set_state_manager(StateManager* sm) { m_state_manager = sm; }
void P2PManager::set_tracker_client(TrackerClient* tc) { m_tracker_client = tc; }  // <-- 新增实现
void P2PManager::set_role(SyncRole role) { m_role = role; }

// (set_turn_config, init, ~P2PManager ... 保持不变 ...)
void P2PManager::set_turn_config(std::string host, uint16_t port, std::string username, std::string password) {
    m_turn_host = std::move(host);
    m_turn_port = port;
    m_turn_username = std::move(username);
    m_turn_password = std::move(password);
    m_turn_server_config.host = m_turn_host.c_str();
    m_turn_server_config.port = m_turn_port;
    m_turn_server_config.username = m_turn_username.c_str();
    m_turn_server_config.password = m_turn_password.c_str();
}
void P2PManager::init() {
    m_thread = std::jthread([this]() {
        g_logger->info("[P2P] IO context 在后台线程运行...");
        auto work_guard = boost::asio::make_work_guard(m_io_context);
        m_io_context.run();
    });
    schedule_kcp_update();
}
P2PManager::~P2PManager() {
    m_io_context.stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (auto& [agent, context] : m_peers_by_agent) {
        if (agent) juice_destroy(agent);
    }
    m_peers_by_agent.clear();
    m_peers_by_id.clear();
}

// (kcp_output_callback, schedule_kcp_update, update_all_kcps ... 保持不变 ...)
int P2PManager::kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user) {
    PeerContext* context = static_cast<PeerContext*>(user);
    if (context && context->agent) {
        if (juice_send(context->agent, buf, len) != 0) {
            // KCP 会重传
        }
    }
    return 0;
}
void P2PManager::schedule_kcp_update() {
    m_kcp_update_timer.expires_after(std::chrono::milliseconds(10));
    m_kcp_update_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->update_all_kcps();
        }
    });
}
void P2PManager::update_all_kcps() {
    auto current_time_ms = (IUINT32)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    std::vector<std::pair<std::string, PeerContext*>> received_messages;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        for (auto const& [agent, context] : m_peers_by_agent) {
            if (!context->kcp) continue;
            ikcp_update(context->kcp, current_time_ms);
            char buffer[BUFFERSIZE];
            int size;
            while ((size = ikcp_recv(context->kcp, buffer, sizeof(buffer))) > 0) {
                received_messages.emplace_back(std::string(buffer, size), context.get());
            }
        }
    }
    for (const auto& msg_pair : received_messages) {
        handle_kcp_message(msg_pair.first, msg_pair.second);
    }
    schedule_kcp_update();
}

// (connect_to_peers, C 回调, C++ 处理器 ... 保持不变 ...)
// src/peer/P2PManager.cpp

void P2PManager::connect_to_peers(const std::vector<std::string>& peer_addresses) {
    std::lock_guard<std::mutex> lock(m_peers_mutex);

    // --- 1. 获取 self_id 用于 tie-breaking ---
    if (!m_tracker_client) {
        g_logger->error("[ICE] TrackerClient is null, 无法获取 self_id 进行 tie-breaking。");
        return;
    }
    std::string self_id = m_tracker_client->get_self_id();
    if (self_id.empty()) {
        g_logger->warn("[ICE] Self ID 尚未设置，推迟连接逻辑。");
        // 这种情况是正常的，如果 Tracker 尚未 ACK
        return;
    }
    // ------------------------------------

    for (const auto& peer_id : peer_addresses) {
        if (m_peers_by_id.count(peer_id)) {
            continue;
        }

        g_logger->info("[ICE] 正在为对等点 {} 创建 ICE Agent...", peer_id);
        juice_config_t config = {};
        if (!m_turn_host.empty()) {
            config.turn_servers = &m_turn_server_config;
            config.turn_servers_count = 1;
        }
        config.user_ptr = this;
        config.cb_state_changed = &P2PManager::on_juice_state_changed;
        config.cb_candidate = &P2PManager::on_juice_candidate;
        config.cb_gathering_done = &P2PManager::on_juice_gathering_done;
        config.cb_recv = &P2PManager::on_juice_recv;
        juice_agent_t* agent = juice_create(&config);
        if (!agent) {
            g_logger->error("[ICE] juice_create 失败 (对等点: {})", peer_id);
            continue;
        }
        auto context = std::make_shared<PeerContext>(peer_id, agent, shared_from_this());
        m_peers_by_agent[agent] = context;
        m_peers_by_id[peer_id] = context;

        // --- 2. 关键修复：TIE-BREAKER 逻辑 ---
        if (self_id < peer_id) {
            // 我们的 ID 较小, 我们是 "Controlling" (控制方)
            g_logger->info("[ICE] Tie-breaking: 我们是 'Controlling' (Offer) 方 (对于 {})", peer_id);
            // 作为 'Controlling', 我们调用 gather_candidates 来生成一个 Offer
            juice_gather_candidates(agent);
        } else {
            // 我们的 ID 较大, 我们是 "Controlled" (受控方)
            g_logger->info("[ICE] Tie-breaking: 我们是 'Controlled' (Answer) 方 (对于 {})。等待 Offer...", peer_id);
            // 作为 'Controlled', 我们什么也不做，
            // 等待 handle_signaling_message 收到 'sdp_offer' 消息。
        }
        // ------------------------------------
    }
}
void P2PManager::on_juice_state_changed(juice_agent_t* agent, juice_state_t state, void* user_ptr) {
    P2PManager* self = static_cast<P2PManager*>(user_ptr);
    boost::asio::post(self->m_io_context, [self, agent, state]() { self->handle_juice_state_changed(agent, state); });
}
void P2PManager::on_juice_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr) {
    P2PManager* self = static_cast<P2PManager*>(user_ptr);
    std::string sdp_str = sdp ? sdp : "";
    boost::asio::post(self->m_io_context,
                      [self, agent, sdp_str]() { self->handle_juice_candidate(agent, sdp_str.c_str()); });
}
void P2PManager::on_juice_gathering_done(juice_agent_t* agent, void* user_ptr) {
    P2PManager* self = static_cast<P2PManager*>(user_ptr);
    boost::asio::post(self->m_io_context, [self, agent]() { self->handle_juice_gathering_done(agent); });
}
void P2PManager::on_juice_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr) {
    P2PManager* self = static_cast<P2PManager*>(user_ptr);
    std::string data_str(data, size);
    boost::asio::post(self->m_io_context,
                      [self, agent, data_str]() { self->handle_juice_recv(agent, data_str.data(), data_str.size()); });
}
void P2PManager::handle_juice_state_changed(juice_agent_t* agent, juice_state_t state) {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    auto it = m_peers_by_agent.find(agent);
    if (it == m_peers_by_agent.end()) return;
    std::string peer_id = it->second->peer_id;
    g_logger->info("[ICE] 对等点 {} 状态改变: {}", peer_id, juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        if (!it->second->kcp) {
            g_logger->info("[KCP] ICE 连接建立，为 {} 设置 KCP 上下文。", peer_id);
            uint32_t conv = static_cast<uint32_t>(std::hash<juice_agent_t*>{}(agent));
            it->second->setup_kcp(conv);
            if (m_role == SyncRole::Source) {
                g_logger->info("[P2P] (Source) KCP 就绪，向新对等点 {} 发送全量状态...", peer_id);
                m_state_manager->scan_directory();
                std::string json_state = m_state_manager->get_state_as_json_string();
                send_over_kcp_peer(json_state, it->second.get());
            }
        }
    } else if (state == JUICE_STATE_FAILED) {
        g_logger->error("[ICE] 对等点 {} 连接失败。", peer_id);
    }
}
void P2PManager::handle_juice_candidate(juice_agent_t* agent, const char* sdp) {
    std::string peer_id;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end()) return;
        peer_id = it->second->peer_id;
    }
    if (!m_tracker_client) return;

    std::string sdp_str = sdp ? sdp : "";
    if (sdp_str.empty()) {
        g_logger->warn("[ICE] handle_juice_candidate 收到空 sdp。");
        return;
    }

    std::string signal_type;

    // --- 关键修复：必须先检查完整的 SDP (Offer/Answer)，再检查 Candidate ---

    // 1. 检查这是否是一个完整的 SDP (Offer/Answer)
    // (特征：包含 "m=" 媒体行 和 "o=" 源行)
    if (sdp_str.find("m=") != std::string::npos && sdp_str.find("o=") != std::string::npos) {
        // 这是一个完整的 SDP。现在判断是 Offer 还是 Answer。
        // "a=setup:actpass" 表示这是一个 Offer
        if (sdp_str.find("a=setup:actpass") != std::string::npos) {
            signal_type = "sdp_offer";
        }
        // "a=setup:active" 或 "a=setup:passive" 表示这是一个 Answer
        else if (sdp_str.find("a=setup:active") != std::string::npos ||
                 sdp_str.find("a=setup:passive") != std::string::npos) {
            signal_type = "sdp_answer";
        } else {
            // 回退：如果找不到 "a=setup" (理论上不应该)，我们根据角色猜测
            std::string self_id = m_tracker_client->get_self_id();
            if (self_id < peer_id) {
                g_logger->warn("[ICE] 找不到 'a=setup' 属性，根据 Tie-Break 猜测为 'sdp_offer'。");
                signal_type = "sdp_offer";  // 我们是 Controlling，所以这是 Offer
            } else {
                g_logger->warn("[ICE] 找不到 'a=setup' 属性，根据 Tie-Break 猜测为 'sdp_answer'。");
                signal_type = "sdp_answer";  // 我们是 Controlled，所以这是 Answer
            }
        }
    }
    // 2. 否则，检查这是否只是一个 ICE Candidate
    else if (sdp_str.find("a=candidate") != std::string::npos) {
        signal_type = "ice_candidate";
    }
    // 3. 否则，我们不知道这是什么
    else {
        g_logger->warn("[ICE] 收到来自 libjuice 的未知信令 (非 candidate 且非 SDP): {}", sdp_str);
        return;
    }

    g_logger->info("[ICE] 为 {} (发送给 {}) 生成本地信令 ({}): {}...", m_tracker_client->get_self_id(), peer_id,
                   signal_type, sdp_str.substr(0, 40));

    // 使用检测到的正确类型发送信令
    m_tracker_client->send_signaling_message(peer_id, signal_type, sdp_str);
}
void P2PManager::handle_juice_gathering_done(juice_agent_t* agent) {
    std::string peer_id;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end()) return;
        peer_id = it->second->peer_id;
    }
    if (!m_tracker_client) return;
    g_logger->info("[ICE] 对等点 {} 的候选地址收集完成。", peer_id);
    // --- 修复：取消注释 ---
    m_tracker_client->send_signaling_message(peer_id, "ice_gathering_done", "");
}
void P2PManager::handle_juice_recv(juice_agent_t* agent, const char* data, size_t size) {
    std::shared_ptr<PeerContext> context;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end() || !it->second->kcp) {
            g_logger->warn("[P2P] 收到来自未知或未就绪 (KCP) Agent 的数据。");
            return;
        }
        context = it->second;
    }
    ikcp_input(context->kcp, data, size);
}
void P2PManager::handle_signaling_message(const std::string& from_peer_id, const std::string& message_type,
                                          const std::string& payload) {
    std::shared_ptr<PeerContext> context;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_id.find(from_peer_id);
        if (it == m_peers_by_id.end()) {
            g_logger->warn("[ICE] 收到来自未知对等点 {} 的信令消息。", from_peer_id);
            return;
        }
        context = it->second;
    }
    g_logger->info("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    if (message_type == "ice_candidate") {
        juice_add_remote_candidate(context->agent, payload.c_str());
    } else if (message_type == "ice_gathering_done") {
        juice_set_remote_gathering_done(context->agent);
    } else if (message_type == "sdp_offer") {
        juice_set_remote_description(context->agent, payload.c_str());
        juice_gather_candidates(context->agent);
    } else if (message_type == "sdp_answer") {
        juice_set_remote_description(context->agent, payload.c_str());
    }
}

// --- 新增：处理 PEER_LEAVE ---
void P2PManager::handle_peer_leave(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    auto it = m_peers_by_id.find(peer_id);
    if (it != m_peers_by_id.end()) {
        g_logger->info("[P2P] 对等点 {} 已断开连接，正在清理...", peer_id);
        juice_agent_t* agent = it->second->agent;

        // 1. 从 agent 映射中移除
        if (agent) {
            m_peers_by_agent.erase(agent);
            juice_destroy(agent);  // 销毁 agent
        }
        // 2. 从 id 映射中移除 (it->second 是 shared_ptr，将在此处被销毁)
        m_peers_by_id.erase(it);
    }
}

// (send_over_kcp, send_over_kcp_peer, handle_kcp_message ... 保持不变 ...)
void P2PManager::send_over_kcp(const std::string& msg) {
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    std::string encrypted_msg = encrypt_gcm(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，广播消息未发送");
        return;
    }
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (auto const& [agent, context] : m_peers_by_agent) {
        if (context->kcp) {
            ikcp_send(context->kcp, encrypted_msg.c_str(), encrypted_msg.length());
        }
    }
}
void P2PManager::send_over_kcp_peer(const std::string& msg, PeerContext* peer) {
    if (!peer || !peer->kcp) {
        g_logger->warn("[KCP] 尝试向无效或未就绪的对等点发送消息。");
        return;
    }
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    std::string encrypted_msg = encrypt_gcm(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，单播消息未发送至 {}", peer->peer_id);
        return;
    }
    ikcp_send(peer->kcp, encrypted_msg.c_str(), encrypted_msg.length());
}
void P2PManager::handle_kcp_message(const std::string& msg, PeerContext* from_peer) {
    std::string decrypted_msg = decrypt_gcm(msg);
    if (decrypted_msg.empty()) {
        return;
    }
    if (decrypted_msg.empty()) {
        g_logger->warn("[KCP] 收到空解密包。");
        return;
    }
    uint8_t msg_type = decrypted_msg[0];
    std::string payload(decrypted_msg.begin() + 1, decrypted_msg.end());
    if (msg_type == MSG_TYPE_JSON) {
        try {
            auto json = nlohmann::json::parse(payload);
            const std::string json_msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
            auto& json_payload = json.at(Protocol::MSG_PAYLOAD);
            if (json_msg_type == Protocol::TYPE_SHARE_STATE && m_role == SyncRole::Destination) {
                handle_share_state(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE && m_role == SyncRole::Destination) {
                handle_file_update(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_DELETE && m_role == SyncRole::Destination) {
                handle_file_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_REQUEST_FILE && m_role == SyncRole::Source) {
                handle_file_request(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_DIR_CREATE && m_role == SyncRole::Destination) {
                handle_dir_create(json_payload);
            } else if (json_msg_type == Protocol::TYPE_DIR_DELETE && m_role == SyncRole::Destination) {
                handle_dir_delete(json_payload, from_peer);
            }
        } catch (const std::exception& e) {
            g_logger->error("[P2P] 处理KCP JSON消息时发生错误: {}", e.what());
            g_logger->error("       原始JSON: {}", payload);
        }
    } else if (msg_type == MSG_TYPE_BINARY_CHUNK) {
        if (m_role == SyncRole::Destination) {
            handle_file_chunk(payload);
        }
    } else {
        g_logger->error("[KCP] 收到未知消息类型: {}", (int)msg_type);
    }
}

// (所有 handle_... 处理器 ... 保持不变 ...)
void P2PManager::handle_share_state(const nlohmann::json& payload, PeerContext* from_peer) {
    if (m_role != SyncRole::Destination) return;
    g_logger->info("[KCP] (Destination) 收到来自 {} (Source) 的 'share_state' 消息。",
                   from_peer ? from_peer->peer_id : std::string("<unknown>"));
    std::vector<FileInfo> remote_files = payload.at("files").get<std::vector<FileInfo>>();
    std::set<std::string> remote_dirs = payload.at("directories").get<std::set<std::string>>();
    m_state_manager->scan_directory();
    nlohmann::json temp_json = nlohmann::json::parse(m_state_manager->get_state_as_json_string());
    std::vector<FileInfo> local_files = temp_json.at(Protocol::MSG_PAYLOAD).at("files").get<std::vector<FileInfo>>();
    std::set<std::string> local_dirs = m_state_manager->get_local_directories();
    g_logger->info("[SyncManager] 正在比较本地目录 ({} 个) 与远程目录 ({} 个).", local_dirs.size(), remote_dirs.size());
    SyncActions file_actions = SyncManager::compare_states_and_get_requests(local_files, remote_files);
    DirSyncActions dir_actions = SyncManager::compare_dir_states(local_dirs, remote_dirs);
    if (!file_actions.files_to_delete.empty()) {
        g_logger->info("[Sync] 计划删除 {} 个本地多余的文件。", file_actions.files_to_delete.size());
        for (const auto& file_path_str : file_actions.files_to_delete) {
            std::filesystem::path relative_path(
                std::u8string_view(reinterpret_cast<const char8_t*>(file_path_str.c_str()), file_path_str.length()));
            std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
            std::error_code ec;
            if (std::filesystem::remove(full_path, ec)) {
                g_logger->info("[Sync] -> 已删除 (相对路径): {}", file_path_str);
            } else if (ec != std::errc::no_such_file_or_directory) {
                g_logger->error("[Sync] -> 删除失败 (相对路径): {} Error: {}", file_path_str, ec.message());
            }
        }
    }
    if (!dir_actions.dirs_to_delete.empty()) {
        g_logger->info("[Sync] 计划删除 {} 个本地多余的目录。", dir_actions.dirs_to_delete.size());
        for (const auto& dir_path_str : dir_actions.dirs_to_delete) {
            std::filesystem::path relative_path(
                std::u8string_view(reinterpret_cast<const char8_t*>(dir_path_str.c_str()), dir_path_str.length()));
            std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
            std::error_code ec;
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
            std::filesystem::path relative_path(
                std::u8string_view(reinterpret_cast<const char8_t*>(dir_path_str.c_str()), dir_path_str.length()));
            std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
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
        g_logger->info("[KCP] 计划向 {} (Source) 请求 {} 个缺失/过期的文件。",
                       from_peer ? from_peer->peer_id : std::string("<unknown>"), file_actions.files_to_request.size());
        for (const auto& file_path : file_actions.files_to_request) {
            nlohmann::json request_msg;
            request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
            request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
            send_over_kcp_peer(request_msg.dump(), from_peer);
        }
    }
}
void P2PManager::handle_file_update(const nlohmann::json& payload, PeerContext* from_peer) {
    if (m_role != SyncRole::Destination) return;
    FileInfo remote_info;
    try {
        remote_info = payload.get<FileInfo>();
    } catch (const std::exception& e) {
        g_logger->error("[KCP] (Destination) 解析 file_update 失败: {}", e.what());
        return;
    }
    g_logger->info("[KCP] (Destination) 收到增量更新: {}", remote_info.path);
    std::filesystem::path relative_path(
        std::u8string_view(reinterpret_cast<const char8_t*>(remote_info.path.c_str()), remote_info.path.length()));
    std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
    std::error_code ec;
    bool should_request = false;
    if (!std::filesystem::exists(full_path, ec) || ec) {
        g_logger->info("[Sync] -> 本地不存在, 需要请求。");
        should_request = true;
    } else {
        std::string local_hash = Hashing::CalculateSHA256(full_path);
        if (local_hash != remote_info.hash) {
            g_logger->info("[Sync] -> 哈希不匹配 (本地: {} vs 远程: {}), 需要请求。", local_hash.substr(0, 7),
                           remote_info.hash.substr(0, 7));
            should_request = true;
        } else {
            g_logger->info("[Sync] -> 哈希匹配, 已是最新。");
        }
    }
    if (should_request) {
        nlohmann::json request_msg;
        request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
        request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
        send_over_kcp_peer(request_msg.dump(), from_peer);
    }
}
void P2PManager::handle_file_delete(const nlohmann::json& payload, PeerContext* from_peer) {
    if (m_role != SyncRole::Destination) return;
    std::string relative_path_str;
    try {
        relative_path_str = payload.at("path").get<std::string>();
    } catch (const std::exception& e) {
        g_logger->error("[KCP] (Destination) 解析 file_delete 失败: {}", e.what());
        return;
    }
    g_logger->info("[KCP] (Destination) 收到增量删除: {} 从 {}", relative_path_str,
                   from_peer ? from_peer->peer_id : std::string("<unknown>"));
    std::filesystem::path relative_path(
        std::u8string_view(reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
    std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
    std::error_code ec;
    if (std::filesystem::remove(full_path, ec)) {
        g_logger->info("[Sync] -> 已删除本地文件 (相对路径): {}", relative_path_str);
        m_state_manager->remove_path_from_map(relative_path_str);
    } else {
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
    } catch (const std::exception& e) {
        g_logger->error("[KCP] (Destination) 解析 dir_create 失败: {}", e.what());
        return;
    }
    g_logger->info("[KCP] (Destination) 收到增量目录创建: {}", relative_path_str);
    std::filesystem::path relative_path(
        std::u8string_view(reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
    std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
    std::error_code ec;
    if (std::filesystem::create_directories(full_path, ec)) {
        g_logger->info("[Sync] -> 已创建目录: {}", relative_path_str);
        m_state_manager->add_dir_to_map(relative_path_str);
    } else if (ec) {
        g_logger->error("[Sync] -> 创建目录失败: {} Error: {}", relative_path_str, ec.message());
    }
}
void P2PManager::handle_dir_delete(const nlohmann::json& payload, PeerContext* from_peer) {
    if (m_role != SyncRole::Destination) return;
    std::string relative_path_str;
    try {
        relative_path_str = payload.at("path").get<std::string>();
    } catch (const std::exception& e) {
        g_logger->error("[KCP] (Destination) 解析 dir_delete 失败: {}", e.what());
        return;
    }
    g_logger->info("[KCP] (Destination) 收到增量目录删除: {} 来自 {}", relative_path_str,
                   from_peer ? from_peer->peer_id : std::string("<unknown>"));
    std::filesystem::path relative_path(
        std::u8string_view(reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
    std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
    std::error_code ec;
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
void P2PManager::handle_file_request(const nlohmann::json& payload, PeerContext* from_peer) {
    const std::string requested_path_str = payload.at("path").get<std::string>();
    g_logger->info("[KCP] 收到来自 {} 对文件 '{}' 的请求。", from_peer ? from_peer->peer_id : std::string("<unknown>"),
                   requested_path_str);
    std::filesystem::path relative_path(
        std::u8string_view(reinterpret_cast<const char8_t*>(requested_path_str.c_str()), requested_path_str.length()));
    std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
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
    auto send_binary_packet = [&](std::string packet_payload) {
        std::string binary_packet;
        binary_packet.push_back(MSG_TYPE_BINARY_CHUNK);
        binary_packet.append(std::move(packet_payload));
        std::string encrypted_msg = encrypt_gcm(binary_packet);
        if (encrypted_msg.empty()) {
            g_logger->error("[KCP] 错误：加密失败，文件块未发送至 {}",
                            from_peer ? from_peer->peer_id : std::string("<unknown>"));
            return;
        }
        if (from_peer && from_peer->kcp) {
            ikcp_send(from_peer->kcp, encrypted_msg.c_str(), encrypted_msg.length());
        }
    };
    if (size == 0) {
        g_logger->info("[KCP] 正在发送零字节文件 '{}' 的元信息...", requested_path_str);
        std::string packet_payload;
        append_uint16(packet_payload, static_cast<uint16_t>(requested_path_str.length()));
        packet_payload.append(requested_path_str);
        append_uint32(packet_payload, 0);
        append_uint32(packet_payload, 1);
        send_binary_packet(std::move(packet_payload));
        return;
    }
    int total_chunks = static_cast<int>((size + CHUNK_DATA_SIZE - 1) / CHUNK_DATA_SIZE);
    std::vector<char> buffer(CHUNK_DATA_SIZE);
    g_logger->info("[KCP] 正在将文件 '{}' ({} 字节) 分成 {} 块 (压缩并) 发送给 {}", requested_path_str, size,
                   total_chunks, from_peer ? from_peer->peer_id : std::string("<unknown>"));
    for (int i = 0; i < total_chunks; ++i) {
        file.read(buffer.data(), CHUNK_DATA_SIZE);
        std::streamsize bytes_read = file.gcount();
        std::string compressed_data;
        snappy::Compress(buffer.data(), bytes_read, &compressed_data);
        std::string packet_payload;
        append_uint16(packet_payload, static_cast<uint16_t>(requested_path_str.length()));
        packet_payload.append(requested_path_str);
        append_uint32(packet_payload, i);
        append_uint32(packet_payload, total_chunks);
        packet_payload.append(compressed_data);
        send_binary_packet(std::move(packet_payload));
    }
}
void P2PManager::handle_file_chunk(const std::string& payload) {
    const char* data_ptr = payload.c_str();
    size_t data_len = payload.length();
    uint16_t path_len = read_uint16(data_ptr, data_len);
    if (path_len == 0 || data_len < path_len) {
        g_logger->error("[KCP] 二进制块解析失败：路径长度无效。");
        return;
    }
    std::string file_path_str(data_ptr, path_len);
    data_ptr += path_len;
    data_len -= path_len;
    uint32_t chunk_index = read_uint32(data_ptr, data_len);
    uint32_t total_chunks = read_uint32(data_ptr, data_len);
    std::string compressed_chunk_data(data_ptr, data_len);
    std::string uncompressed_data;
    if (data_len == 0) {
    } else if (!snappy::Uncompress(compressed_chunk_data.data(), compressed_chunk_data.size(), &uncompressed_data)) {
        g_logger->error("[KCP] Snappy 解压失败 (包可能已损坏): {}", file_path_str);
        return;
    }
    auto& assembly_info = m_file_assembly_buffer[file_path_str];
    assembly_info.first = total_chunks;
    assembly_info.second[chunk_index] = std::move(uncompressed_data);
    g_logger->debug("[KCP] 收到文件 '{}' 的块 {}/{} (压缩后: {} 字节, 解压后: {} 字节).", file_path_str,
                    chunk_index + 1, total_chunks, compressed_chunk_data.size(),
                    assembly_info.second[chunk_index].size());
    if (assembly_info.second.size() == total_chunks) {
        g_logger->info("[KCP] 文件 '{}' 的所有块已收齐，正在重组...", file_path_str);
        std::filesystem::path relative_path(
            std::u8string_view(reinterpret_cast<const char8_t*>(file_path_str.c_str()), file_path_str.length()));
        std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;
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
            output_file.write(assembly_info.second[i].data(), assembly_info.second[i].length());
        }
        output_file.close();
        g_logger->info("[P2P] 成功: 文件 '{}' 已保存。", file_path_str);
        m_file_assembly_buffer.erase(file_path_str);
    }
}

}  // namespace VeritasSync
