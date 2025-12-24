#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "VeritasSync/p2p/P2PManager.h"

#include <httplib.h>

#include <algorithm>
#include <boost/asio/ip/udp.hpp>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#include "VeritasSync/common/EncodingUtils.h"
#include "VeritasSync/common/Hashing.h"
#include "VeritasSync/common/Logger.h"
#include "VeritasSync/sync/Protocol.h"
#include "VeritasSync/storage/StateManager.h"
#include "VeritasSync/sync/SyncManager.h"
#include "VeritasSync/p2p/TrackerClient.h"
#define BUFFERSIZE 32768
#include <b64/decode.h>
#include <b64/encode.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/asio/detail/socket_ops.hpp>

namespace VeritasSync {

bool can_broadcast(SyncRole role, SyncMode mode) {
    // 如果是 Source，永远可以广播
    if (role == SyncRole::Source) return true;
    // 如果是双向模式，Destination 也可以广播
    if (mode == SyncMode::BiDirectional) return true;
    // 否则(单向模式下的 Destination) 禁止广播
    return false;
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

PeerContext::PeerContext(std::string id, juice_agent_t* ag, std::shared_ptr<P2PManager> manager_ptr)
    : peer_id(std::move(id)), agent(ag), p2p_manager_ptr(std::move(manager_ptr)), connected_at_ts(0) {}

PeerContext::~PeerContext() {
    if (kcp) {
        ikcp_release(kcp);
        kcp = nullptr;
    }
}
void PeerContext::setup_kcp(uint32_t conv) {
    kcp = ikcp_create(conv, this);
    kcp->output = &P2PManager::kcp_output_callback;

    // --- [优化 1] 极速模式配置 ---
    // 参考文档：协议配置 -> 工作模式
    // nodelay: 1 (启用)
    // interval: 5 (内部时钟 5ms，比默认 10ms 更激进，响应更快)
    // resend: 2 (快速重传，2次跨越即重传)
    // nc: 1 (关闭流控，这对跑满带宽至关重要)
    ikcp_nodelay(kcp, 1, 5, 2, 1);

    // --- [优化 2] 扩大收发窗口 ---
    // 参考文档：重设窗口大小 -> kcptun默认1024
    // 我们设为 4096，约允许 5.6MB 数据在途。
    // 理论速度：5.6MB / 0.2s RTT = 28MB/s
    ikcp_wndsize(kcp, 4096, 4096);

    // --- [优化 3] 降低最小 RTO ---
    // 参考文档：如果你还想更激进 -> 确认 minrto
    // 默认 30ms-100ms，改为 10ms，在网络抖动时恢复更快
    kcp->rx_minrto = 10;
}

void P2PManager::set_encryption_key(const std::string& key_string) { m_crypto.set_key(key_string); }

static const int GCM_IV_LEN = 12;
static const int GCM_TAG_LEN = 16;

boost::asio::io_context& P2PManager::get_io_context() { return m_io_context; }

void P2PManager::broadcast_current_state() {
    if (!can_broadcast(m_role, m_mode)) return;
    if (!m_state_manager) return;

    // 异步投递任务到线程池，避免阻塞主网络线程
    boost::asio::post(m_worker_pool, [self = shared_from_this()]() {
        // [Worker线程] 1. 执行耗时的文件扫描
        self->m_state_manager->scan_directory();
        std::string json_state = self->m_state_manager->get_state_as_json_string();

        // [Worker线程] 2. 预先进行加密 (耗时操作)
        // 注意：m_encryption_key 在启动后只读，因此这里是线程安全的
        std::string json_packet;
        json_packet.push_back(MSG_TYPE_JSON);
        json_packet.append(json_state);

        std::string encrypted_msg = self->m_crypto.encrypt(json_packet);

        if (encrypted_msg.empty()) {
            g_logger->error("[Worker] 加密失败，放弃广播。");
            return;
        }

        // [Main线程] 3. 切回 IO 线程发送数据
        // 因为 m_peers_by_agent 和 kcp 均非线程安全，必须在 io_context 线程操作
        boost::asio::post(self->m_io_context, [self, encrypted_msg = std::move(encrypted_msg)]() {
            std::lock_guard<std::mutex> lock(self->m_peers_mutex);
            int sent_count = 0;
            for (auto const& [agent, context] : self->m_peers_by_agent) {
                if (context->kcp) {
                    ikcp_send(context->kcp, encrypted_msg.c_str(), encrypted_msg.length());
                    sent_count++;
                }
            }
            if (sent_count > 0) {
                g_logger->info("[P2P] (Source) 广播状态完成 (发送给 {} 个对等点)", sent_count);
            }
        });
    });
}
void P2PManager::broadcast_file_update(const FileInfo& file_info) {
    if (!can_broadcast(m_role, m_mode)) return;
    g_logger->info("[P2P] (Source) 广播增量更新: {}", file_info.path);
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
    msg[Protocol::MSG_PAYLOAD] = file_info;
    send_over_kcp(msg.dump());
}
void P2PManager::broadcast_file_delete(const std::string& relative_path) {
    if (!can_broadcast(m_role, m_mode)) return;
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

// --- 静态工厂与构造函数  ---
std::shared_ptr<P2PManager> P2PManager::create() {
    struct P2PManagerMaker : public P2PManager {
        P2PManagerMaker() : P2PManager() {}
    };
    auto manager = std::make_shared<P2PManagerMaker>();
    manager->m_last_data_time = std::chrono::steady_clock::now();  // 初始化时间
    manager->init();
    return manager;
}

P2PManager::P2PManager()
    : m_io_context(),
      m_kcp_update_timer(m_io_context),
      m_cleanup_timer(m_io_context),
      m_worker_pool(std::thread::hardware_concurrency()) {
    // --- 只记录重要的 libjuice 日志 (INFO 及以上级别) ---
    juice_set_log_level(JUICE_LOG_LEVEL_INFO);  // 关闭 DEBUG 和 VERBOSE
    juice_set_log_handler([](juice_log_level_t level, const char* message) {
        switch (level) {
            case JUICE_LOG_LEVEL_INFO:
                // 过滤掉一些噪音日志
                if (std::string_view(message).find("Changing state to") != std::string_view::npos ||
                    std::string_view(message).find("Candidate gathering done") != std::string_view::npos ||
                    std::string_view(message).find("Connectivity timer") != std::string_view::npos) {
                    g_logger->info("[libjuice] {}", message);
                }
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

void P2PManager::set_state_manager(StateManager* sm) {
    m_state_manager = sm;
    if (m_transfer_manager) {
        m_transfer_manager->set_state_manager(sm);
    }
}
void P2PManager::set_tracker_client(TrackerClient* tc) { m_tracker_client = tc; }
void P2PManager::set_role(SyncRole role) { m_role = role; }

void P2PManager::set_stun_config(std::string host, uint16_t port) {
    m_stun_host = std::move(host);
    m_stun_port = port;
    g_logger->info("[Config] STUN 服务器设置为: {}:{}", m_stun_host, m_stun_port);
}

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
    auto send_cb = [weak_self = weak_from_this()](const std::string& peer_id,
                                                  const std::string& encrypted_data) -> int {
        auto self = weak_self.lock();
        if (!self) return 0;

        // 加锁：保护 KCP 结构体，防止与 IO 线程的 ikcp_update 冲突
        std::lock_guard<std::mutex> lock(self->m_peers_mutex);

        auto it = self->m_peers_by_id.find(peer_id);
        if (it != self->m_peers_by_id.end() && it->second->kcp) {
            // 1. 同步发送：直接将数据放入 KCP 发送队列 (内存操作，非常快)
            // 这一步必须同步，否则 waitsnd 无法立即反映积压量
            ikcp_send(it->second->kcp, encrypted_data.c_str(), encrypted_data.length());

            // 2. 立即获取最新的积压量 (包括刚刚放入的包)
            // 这将作为返回值告诉 TransferManager 是否需要减速
            return ikcp_waitsnd(it->second->kcp);
        }
        return 0;
    };

    // 创建实例
    m_transfer_manager = std::make_shared<TransferManager>(m_state_manager, m_worker_pool, m_crypto, send_cb);
    m_thread = std::jthread([this]() {
        g_logger->info("[P2P] IO context 在后台线程运行...");
        auto work_guard = boost::asio::make_work_guard(m_io_context);
        m_io_context.run();
    });
    schedule_kcp_update();
    schedule_cleanup_task();  // 启动清理任务
    init_upnp();              // 启动 UPnP 发现
}
P2PManager::~P2PManager() {
    m_io_context.stop();
    m_worker_pool.join();
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

int P2PManager::kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user) {
    PeerContext* context = static_cast<PeerContext*>(user);
    // 【关键】检查连接有效性，避免 use-after-free
    if (context && context->is_valid.load() && context->agent) {
        if (juice_send(context->agent, buf, len) != 0) {
            // KCP 会重传
        }
    }
    return 0;
}
void P2PManager::schedule_kcp_update() {
    m_kcp_update_timer.expires_after(std::chrono::milliseconds(m_kcp_update_interval_ms));
    m_kcp_update_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->update_all_kcps();
        }
    });
}
void P2PManager::update_all_kcps() {
    // 获取当前时间戳用于驱动 KCP 时钟
    auto current_time_ms = (IUINT32)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

    // 临时存储收到的消息，避免在锁内处理业务逻辑
    std::vector<std::pair<std::string, std::shared_ptr<PeerContext>>> received_messages;

    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        for (auto const& [agent, context] : m_peers_by_agent) {
            if (!context->kcp) continue;

            // 1. 驱动 KCP 状态机 (处理重传、拥塞控制等)
            ikcp_update(context->kcp, current_time_ms);

            // 2. 动态接收循环
            // KCP 可能积压了多个包，所以要用 while 循环一次性收完
            while (true) {
                // [关键步骤 A] 偷看下一个包的大小
                int peek_size = ikcp_peeksize(context->kcp);

                // 如果 peek_size < 0，说明没有完整的数据包，直接跳出循环
                if (peek_size < 0) break;

                // [关键步骤 B] 根据包大小动态分配内存
                // std::vector 会在栈上管理对象，在堆上分配 peek_size 大小的内存
                // 如果包是 700KB，这里就分配 700KB；如果是 50字节，就分配 50字节
                std::vector<char> buffer(peek_size);

                // [关键步骤 C] 真正接收数据
                // 此时 buffer 大小完全匹配，绝对不会失败
                int recv_len = ikcp_recv(context->kcp, buffer.data(), peek_size);

                if (recv_len > 0) {
                    // 将接收到的二进制数据转为 string，存入暂存队列
                    // 这里的 buffer.data() 包含了完整的 JSON 快照或文件块
                    received_messages.emplace_back(std::string(buffer.data(), recv_len), context);
                } else {
                    // 理论上 peek 成功后 recv 不应失败，防止死循环
                    break;
                }
            }
        }
    }

    // --- 以下是自适应更新频率逻辑 (保持原样) ---
    if (!received_messages.empty()) {
        m_last_data_time = std::chrono::steady_clock::now();
        // 有数据传输时，全力驱动 KCP，间隔设为 5ms 以降低延迟
        m_kcp_update_interval_ms = 5;
    } else {
        // 空闲时降低 CPU 占用
        auto idle_duration = std::chrono::steady_clock::now() - m_last_data_time;
        if (idle_duration > std::chrono::seconds(5)) {
            m_kcp_update_interval_ms = 100;  // 深度睡眠
        } else {
            m_kcp_update_interval_ms = 10;  // 轻度睡眠
        }
    }

    // --- 处理消息 (在锁外执行，防止死锁) ---
    for (const auto& msg_pair : received_messages) {
        // msg_pair.first 是消息内容 string
        // msg_pair.second 是 PeerContext 指针
        handle_kcp_message(msg_pair.first, msg_pair.second.get());
    }

    // 调度下一次更新
    schedule_kcp_update();
}

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

        g_logger->info("[ICE] 正在为对等点 {} 创建ICE Agent...", peer_id);
        juice_config_t config = {};

        // --- 配置STUN服务器 (关键！) ---
        if (!m_stun_host.empty()) {
            config.stun_server_host = m_stun_host.c_str();
            config.stun_server_port = m_stun_port;
            g_logger->debug("[ICE] 对等点 {} 使用 STUN: {}:{}", peer_id, m_stun_host, m_stun_port);
        }
        // ----------------------------------

        // --- 配置TURN服务器 ---
        if (!m_turn_host.empty()) {
            config.turn_servers = &m_turn_server_config;
            config.turn_servers_count = 1;
            g_logger->debug("[ICE] 对等点 {} 使用 TURN: {}:{}", peer_id, m_turn_host, m_turn_port);
        }
        // ----------------------------------

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

        // --- 2. TIE-BREAKER 逻辑 ---
        if (self_id < peer_id) {
            // 我们的 ID 较小, 我们是 "Controlling" (控制方)
            g_logger->info("[ICE] Tie-breaking: 我们是 'Controlling' (Offer) 方 (对于 {})", peer_id);
            // 作为 'Controlling', 我们需要：
            // 1. 收集候选地址 (这将触发 cb_candidate 回调)
            juice_gather_candidates(agent);
            // 2. 在收集完成后 (cb_gathering_done), 获取并发送完整的 SDP Offer
            //    (这部分逻辑在 handle_juice_gathering_done 中实现)
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
    std::shared_ptr<PeerContext> context;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end()) return;
        context = it->second;
    }
    std::string peer_id = context->peer_id;

    g_logger->info("[ICE] 对等点 {} 状态改变: {}", peer_id, juice_state_to_string(state));

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        // --- 查询当前的连接类型 ---
        char local_sdp[1024];
        char remote_sdp[1024];
        ConnectionType new_type = ConnectionType::None;

        // 获取当前选定的候选地址对
        if (juice_get_selected_candidates(agent, local_sdp, sizeof(local_sdp), remote_sdp, sizeof(remote_sdp)) == 0) {
            std::string local_str(local_sdp);
            // 检查本地候选地址的类型
            if (local_str.find(" typ host") != std::string::npos || local_str.find(" typ srflx") != std::string::npos) {
                new_type = ConnectionType::P2P;
            } else if (local_str.find(" typ relay") != std::string::npos) {
                new_type = ConnectionType::Relay;
            }

            g_logger->debug("[ICE] 候选地址: {}", local_str);
        }
        // -----------------------------------

        // --- 检查连接类型是否发生变化 ---
        if (new_type != ConnectionType::None && new_type != context->current_type) {
            if (new_type == ConnectionType::Relay) {
                // Relay 中继连接建立
                g_logger->warn("[ICE] 连接已建立：当前使用 UDP Relay (中继服务器) -> {}", peer_id);
                g_logger->warn("[ICE] 正在后台尝试 P2P 直连优化...");
            } else if (new_type == ConnectionType::P2P) {
                if (context->current_type == ConnectionType::Relay) {
                    // 从 Relay 升级到 P2P
                    g_logger->info("[ICE] ✨ P2P升级成功！连接已自动切换到 UDP P2P (直连) -> {}", peer_id);
                    g_logger->info("[ICE] 流量已不再经过 TURN 服务器，节省带宽成本。");
                } else {
                    // 直接 P2P 连接成功
                    g_logger->info("[ICE] 连接已建立：直接 P2P 直连 -> {}", peer_id);
                }
            }
            context->current_type = new_type;
        }
        // -----------------------------------

        if (!context->kcp) {
            // 获取当前系统时间戳 (秒)
            auto now = std::chrono::system_clock::now();
            context->connected_at_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

            // 打印日志方便调试
            g_logger->info("[P2P] 会话建立时间戳: {}", context->connected_at_ts);
            std::string conn_type_str = (new_type == ConnectionType::P2P)     ? "P2P"
                                        : (new_type == ConnectionType::Relay) ? "Relay"
                                                                              : "Unknown";
            g_logger->info("[KCP] ICE 连接建立 ({})，为 {} 设置 KCP 上下文。", conn_type_str, peer_id);

            std::string self_id = m_tracker_client ? m_tracker_client->get_self_id() : "";
            std::string id_pair = (self_id < peer_id) ? (self_id + peer_id) : (peer_id + self_id);
            uint32_t conv = static_cast<uint32_t>(std::hash<std::string>{}(id_pair));

            g_logger->info("[KCP] 使用 conv ID: {} (基于: {} <-> {})", conv, self_id, peer_id);
            context->setup_kcp(conv);

            // --- 【1.2版本】可靠同步会话机制 ---
            // 仅在首次建立 KCP 连接时触发同步推送
            if (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional) {
                g_logger->info("[P2P] 连接激活 ({})，准备向对等点 {} 推送文件状态...", juice_state_to_string(state),
                               peer_id);

                // 生成新的同步会话 ID
                uint64_t session_id = std::chrono::steady_clock::now().time_since_epoch().count();
                context->sync_session_id.store(session_id);

                // 投递到 Worker 线程执行同步
                boost::asio::post(m_worker_pool, [this, self = shared_from_this(), ctx = context, session_id]() {
                    perform_flood_sync(ctx, session_id);
                });
            }
        }
        // ---------------------------------

        // --- 连接成功时重置重连计数 ---
        {
            std::lock_guard<std::mutex> reconnect_lock(m_reconnect_mutex);
            m_reconnect_attempts.erase(peer_id);
            m_last_reconnect_time.erase(peer_id);
        }
        // ---------------------------------

    } else if (state == JUICE_STATE_FAILED) {
        // --- ICE FAILED 自动重连机制 ---
        g_logger->error("[ICE] 对等点 {} P2P/Relay 均连接失败，将清理并尝试重连...", peer_id);

        // 1. 清理失败的连接
        handle_peer_leave(peer_id);

        // 2. 检查重试次数并调度重连
        schedule_reconnect(peer_id);
        // ---------------------------------
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
    if (sdp_str.empty()) return;

    std::string candidate_to_send = sdp_str;

    // 字符串健壮性处理：移除尾部换行符 (这是必要的，防止 JSON 格式错误)
    while (!candidate_to_send.empty() && (candidate_to_send.back() == '\r' || candidate_to_send.back() == '\n')) {
        candidate_to_send.pop_back();
    }

    //  UPnP 重写 (这个是有益的优化，建议保留)
    std::string final_candidate = rewrite_candidate(candidate_to_send);

    g_logger->info("[ICE] 发送候选: {}...", final_candidate.substr(0, 50));
    m_tracker_client->send_signaling_message(peer_id, "ice_candidate", final_candidate);
}
void P2PManager::handle_juice_gathering_done(juice_agent_t* agent) {
    std::string peer_id;
    std::string self_id;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end()) return;
        peer_id = it->second->peer_id;
    }
    if (!m_tracker_client) return;

    self_id = m_tracker_client->get_self_id();
    g_logger->info("[ICE] 对等点 {} 的候选地址收集完成。", peer_id);

    char local_description[4096];
    if (juice_get_local_description(agent, local_description, sizeof(local_description)) == 0) {
        std::string sdp_str(local_description);

        if (self_id < peer_id) {
            g_logger->info("[ICE] 向 {} 发送 SDP Offer ({} 字节)", peer_id, sdp_str.length());
            m_tracker_client->send_signaling_message(peer_id, "sdp_offer", sdp_str);
        } else {
            g_logger->info("[ICE] 向 {} 发送 SDP Answer ({} 字节)", peer_id, sdp_str.length());
            m_tracker_client->send_signaling_message(peer_id, "sdp_answer", sdp_str);
        }
    } else {
        g_logger->error("[ICE] 获取本地 SDP 描述失败 (对等点: {})", peer_id);
    }

    m_tracker_client->send_signaling_message(peer_id, "ice_gathering_done", "");
}
void P2PManager::handle_juice_recv(juice_agent_t* agent, const char* data, size_t size) {
    std::shared_ptr<PeerContext> context;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        auto it = m_peers_by_agent.find(agent);
        if (it == m_peers_by_agent.end()) {
            g_logger->warn("[P2P] 收到来自未知 Agent 的数据 ({} bytes)。", size);
            return;
        }
        if (!it->second->kcp) {
            g_logger->debug("[P2P] 收到数据但 KCP 未就绪 (对等点: {}, {} bytes) - 忽略。", it->second->peer_id, size);
            return;
        }
        context = it->second;
    }
    // 移除调试日志，减少噪音
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
    if (message_type == "ice_candidate") {
        g_logger->debug("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    } else {
        g_logger->info("[ICE] 收到来自 {} 的信令: {}", from_peer_id, message_type);
    }
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

void P2PManager::handle_peer_leave(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    auto it = m_peers_by_id.find(peer_id);
    if (it != m_peers_by_id.end()) {
        g_logger->info("[P2P] 对等点 {} 已断开连接，正在清理...", peer_id);
        
        auto context = it->second;
        
        // 1. 【关键】先标记为无效，让所有异步任务（flood等）安全退出
        context->is_valid.store(false);
        
        // 2. 清理 kcp
        if (context->kcp) {
            ikcp_release(context->kcp);
            context->kcp = nullptr;
        }
        
        // 3. 清理 agent
        juice_agent_t* agent = context->agent;
        if (agent) {
            m_peers_by_agent.erase(agent);
            context->agent = nullptr;  // 先置空，防止悬空指针
            juice_destroy(agent);
        }
        
        // 4. 从 id 映射中移除
        m_peers_by_id.erase(it);
    }
}

void P2PManager::schedule_reconnect(const std::string& peer_id) {
    int attempt_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_reconnect_mutex);
        attempt_count = ++m_reconnect_attempts[peer_id];

        // 达到最大重试次数，停止重连
        if (attempt_count > MAX_RECONNECT_ATTEMPTS) {
            g_logger->error("[ICE] 对等点 {} 已达最大重连次数 ({})，停止重试。请检查网络或在 WebUI 重启服务。", peer_id,
                            MAX_RECONNECT_ATTEMPTS);
            m_reconnect_attempts.erase(peer_id);
            return;
        }

        m_last_reconnect_time[peer_id] = std::chrono::steady_clock::now();
    }

    // 指数退避延迟: 3s, 6s, 12s, 24s, 48s
    int delay_ms = BASE_RECONNECT_DELAY_MS * (1 << (attempt_count - 1));
    g_logger->info("[ICE] 将在 {} ms 后尝试重新连接对等点 {} (第 {} 次尝试)", delay_ms, peer_id, attempt_count);

    // 创建定时器延迟重连
    auto timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    timer->expires_after(std::chrono::milliseconds(delay_ms));
    timer->async_wait([this, self = shared_from_this(), peer_id, timer](const boost::system::error_code& ec) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                g_logger->warn("[ICE] 重连定时器取消/错误: {} (peer: {})", ec.message(), peer_id);
            }
            return;
        }

        // 检查是否已经重新连接成功（在等待期间）
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            if (m_peers_by_id.find(peer_id) != m_peers_by_id.end()) {
                g_logger->info("[ICE] 对等点 {} 已重新连接，跳过本次重连。", peer_id);
                return;
            }
        }

        g_logger->info("[ICE] 开始重新连接对等点 {}...", peer_id);
        connect_to_peers({peer_id});
    });
}

void P2PManager::send_over_kcp(const std::string& msg) {
    std::string json_packet;
    json_packet.push_back(MSG_TYPE_JSON);
    json_packet.append(msg);
    std::string encrypted_msg = m_crypto.encrypt(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，广播消息未发送");
        return;
    }
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    int sent_count = 0;
    for (auto const& [agent, context] : m_peers_by_agent) {
        if (context->kcp) {
            ikcp_send(context->kcp, encrypted_msg.c_str(), encrypted_msg.length());
            sent_count++;
        }
    }
    // 只在有消息时记录 info 级别
    if (sent_count > 0) {
        g_logger->info("[KCP] 广播消息到 {} 个对等点 ({} bytes)", sent_count, encrypted_msg.length());
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
    std::string encrypted_msg = m_crypto.encrypt(json_packet);
    if (encrypted_msg.empty()) {
        g_logger->error("[KCP] 错误：加密失败，单播消息未发送至 {}", peer->peer_id);
        return;
    }
    ikcp_send(peer->kcp, encrypted_msg.c_str(), encrypted_msg.length());
}

// 增加新的线程安全 wrapper
void P2PManager::send_over_kcp_peer_safe(const std::string& msg,  const std::string& peer_id) {
     std::lock_guard<std::mutex> lock(m_peers_mutex);
     auto it = m_peers_by_id.find(peer_id);
     if (it != m_peers_by_id.end() && it->second->kcp) {
          send_over_kcp_peer(msg, it->second.get());
     }
}

void P2PManager::handle_kcp_message(const std::string& msg, PeerContext* from_peer) {
    std::string decrypted_msg = m_crypto.decrypt(msg);
    if (decrypted_msg.empty()) {
        g_logger->warn("[KCP] 解密失败 ({} bytes 原始数据)", msg.size());
        return;
    }
    uint8_t msg_type = decrypted_msg[0];
    std::string payload(decrypted_msg.begin() + 1, decrypted_msg.end());

    bool can_receive = m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional;
    if (msg_type == MSG_TYPE_JSON) {
        try {
            auto json = nlohmann::json::parse(payload);
            const std::string json_msg_type = json.at(Protocol::MSG_TYPE).get<std::string>();
            auto& json_payload = json.at(Protocol::MSG_PAYLOAD);

            // 只记录重要的消息类型
            g_logger->info("[KCP] 收到 '{}' 消息 (来自: {})", json_msg_type,
                           from_peer ? from_peer->peer_id : "<unknown>");

            if (json_msg_type == Protocol::TYPE_SHARE_STATE && can_receive) {
                handle_share_state(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_UPDATE && can_receive) {
                handle_file_update(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_FILE_DELETE && can_receive) {
                handle_file_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_REQUEST_FILE) {
                bool can_serve = (m_role == SyncRole::Source || m_mode == SyncMode::BiDirectional);
                if (can_serve && from_peer) {
                    m_transfer_manager->queue_upload(from_peer->peer_id, json_payload);
                }
            } else if (json_msg_type == Protocol::TYPE_DIR_CREATE && can_receive) {
                handle_dir_create(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_DIR_DELETE && can_receive) {
                handle_dir_delete(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_SYNC_BEGIN && can_receive) {
                handle_sync_begin(json_payload, from_peer);
            } else if (json_msg_type == Protocol::TYPE_SYNC_ACK) {
                // ACK 可以由任何角色处理
                handle_sync_ack(json_payload, from_peer);
            } else {
                g_logger->warn("[KCP] 消息类型 '{}' 不适用于当前角色 ({})", json_msg_type,
                               m_role == SyncRole::Source ? "Source" : "Destination");
            }
        } catch (const std::exception& e) {
            g_logger->error("[P2P] 处理KCP JSON消息时发生错误: {}", e.what());
        }
    } else if (msg_type == MSG_TYPE_BINARY_CHUNK) {
        //  允许双向传输 (Destination 或 双向模式 均可接收数据)
        if (m_role == SyncRole::Destination || m_mode == SyncMode::BiDirectional) {
            // [修改] 传入 peer_id (from_peer 可能为 nullptr，需判空)
            std::string sender_id = from_peer ? from_peer->peer_id : "";
            m_transfer_manager->handle_chunk(payload, sender_id);
        }
    } else {
        g_logger->error("[KCP] 收到未知消息类型: {}", (int)msg_type);
    }
}
TransferManager::SessionStats P2PManager::get_transfer_stats() {
    if (m_transfer_manager) {
        return m_transfer_manager->get_session_stats();
    }
    return {0, 0};
}
void P2PManager::handle_share_state(const nlohmann::json& payload, PeerContext* from_peer) {
    // 1. 角色检查
    if (m_role != SyncRole::Destination && m_mode != SyncMode::BiDirectional) return;

    // 2. 获取 PeerID 和 连接时间
    std::string peer_id = from_peer ? from_peer->peer_id : "";
    if (peer_id.empty()) return;

    // 【核心修复】计算历史记录的"安全阈值"
    // 逻辑：连接建立时间 - 5秒缓冲。
    // 任何晚于 (连接时间-5s) 的数据库记录，都认为是连接建立后产生的新交互，
    // 不能用来评估对方发来的这份（可能是在连接建立瞬间生成的）旧快照。
    int64_t safe_threshold_ts = from_peer ? (from_peer->connected_at_ts - 5) : 0;

    g_logger->info("[KCP] (Destination) 收到来自 {} 的状态。连接TS: {}, 历史阈值: {}", peer_id,
                   from_peer->connected_at_ts, safe_threshold_ts);

    // 3. 投递到 Worker 线程池处理 (避免阻塞网络线程)
    boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload, peer_id, safe_threshold_ts]() {
        // --- 以下逻辑在后台线程执行 ---

        // A. 解析远程状态
        std::vector<FileInfo> remote_files;
        std::set<std::string> remote_dirs;
        try {
            remote_files = payload.at("files").get<std::vector<FileInfo>>();
            remote_dirs = payload.at("directories").get<std::set<std::string>>();
        } catch (const std::exception& e) {
            g_logger->error("[Sync] 解析 share_state 失败: {}", e.what());
            return;
        }

        // B. 扫描本地状态 (IO 密集型)
        self->m_state_manager->scan_directory();

        // C. 获取本地状态数据
        // 注意：这里涉及 JSON 序列化/反序列化以获取深拷贝
        nlohmann::json temp_json = nlohmann::json::parse(self->m_state_manager->get_state_as_json_string());
        std::vector<FileInfo> local_files =
            temp_json.at(Protocol::MSG_PAYLOAD).at("files").get<std::vector<FileInfo>>();
        std::set<std::string> local_dirs = self->m_state_manager->get_local_directories();

        g_logger->info("[SyncManager] 正在比较本地目录 ({} 个) 与远程目录 ({} 个).", local_files.size(),
                       remote_files.size());

        // D. 执行比较算法 (CPU 密集型)
        // 【关键】构建智能拦截回调
        auto history_func = [self, peer_id, safe_threshold_ts](const std::string& path) -> std::optional<SyncHistory> {
            // 1. 查数据库获取完整历史 (Hash + Timestamp)
            auto res = self->m_state_manager->get_full_history(peer_id, path);

            if (res.has_value()) {
                // 2. 检查时间戳：如果记录时间 > 连接时间阈值
                if (res->ts > safe_threshold_ts) {
                    // 说明这是一条"未来"记录（相对于快照生成时间），是连接建立后才产生的。
                    // 触发快照隔离：假装这条历史不存在，强制 SyncManager 认为这是"新增文件"从而保留它。
                    g_logger->info("[Sync] 🛡️ 忽略过新历史: {} (记录TS {} > 阈值 {}), 判定为状态滞后，强制保留。", path,
                                   res->ts, safe_threshold_ts);
                    return std::nullopt;
                }
            }
            return res;
        };

        // 调用 SyncManager (SyncManager::compare_states_and_get_requests 需已更新签名)
        SyncActions file_actions =
            SyncManager::compare_states_and_get_requests(local_files, remote_files, history_func, self->m_mode);

        DirSyncActions dir_actions = SyncManager::compare_dir_states(local_dirs, remote_dirs, self->m_mode);

        // E. 执行批量 IO 操作 (删除/创建)
        if (!file_actions.files_to_conflict_rename.empty()) {
            g_logger->info("[Sync] 处理 {} 个冲突文件...", file_actions.files_to_conflict_rename.size());

            for (const auto& rel_path : file_actions.files_to_conflict_rename) {
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(rel_path);

                // 构造冲突文件名: filename.conflict.YYYYMMDD-HHMMSS.ext
                auto now = std::chrono::system_clock::now();
                std::time_t now_c = std::chrono::system_clock::to_time_t(now);
                struct tm timeinfo;

                // 跨平台获取本地时间
#ifdef _WIN32
                localtime_s(&timeinfo, &now_c);
#else
                localtime_r(&now_c, &timeinfo);
#endif

                // 格式化时间字符串
                char time_buf[64];
                std::strftime(time_buf, sizeof(time_buf), "%Y%m%d-%H%M%S", &timeinfo);

                std::string stem = full_path.stem().string();
                std::string ext = full_path.extension().string();

                // 新的命名方式，例如: test.conflict.20251205-194033.txt
                std::string conflict_name = stem + ".conflict." + std::string(time_buf) + ext;
                std::filesystem::path conflict_path = full_path.parent_path() / conflict_name;

                std::error_code ec;
                std::filesystem::rename(full_path, conflict_path, ec);

                if (!ec) {
                    g_logger->warn("[Sync] ⚡ 已保留冲突副本: {} -> {}", rel_path, conflict_path.filename().string());
                    // 【重要】重命名后，原路径的文件就"消失"了。
                    // 随后执行的 request_file 会重新下载远程版本填补这个空缺。
                    // 同时也需要通知 StateManager 移除旧记录，防止干扰
                    // (虽然不移也行，因为会被覆盖，但移除更干净)
                    // self->m_state_manager->remove_path_from_map(rel_path);
                } else {
                    g_logger->error("[Sync] ❌ 冲突重命名失败: {} | {}", rel_path, FormatErrorCode(ec));
                }
            }
        }

        // E1. 删除多余文件
        if (!file_actions.files_to_delete.empty()) {
            g_logger->info("[Sync] 计划删除 {} 个本地多余的文件。", file_actions.files_to_delete.size());
            for (const auto& file_path_str : file_actions.files_to_delete) {
                std::filesystem::path relative_path = Utf8ToPath(file_path_str);
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;
                std::error_code ec;

                // 执行删除
                if (std::filesystem::remove(full_path, ec)) {
                    g_logger->info("[Sync] -> 已删除 (相对路径): {}", file_path_str);
                    // 【重要】删除成功后，必须从 StateManager 和 数据库历史 中移除
                    // 这样下次如果重新创建该文件，就不会因为有旧历史而被判定为"删除"
                    self->m_state_manager->remove_path_from_map(file_path_str);
                } else if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", file_path_str, FormatErrorCode(ec));
                }
            }
        }

        // E2. 删除多余目录
        if (!dir_actions.dirs_to_delete.empty()) {
            // 排序优化：由深到浅删除，避免父目录先被删导致子目录删除失败
            std::vector<std::string> sorted_dirs = dir_actions.dirs_to_delete;
            std::sort(sorted_dirs.begin(), sorted_dirs.end(),
                      [](const std::string& a, const std::string& b) { return a.length() > b.length(); });

            for (const auto& dir_path_str : sorted_dirs) {
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                bool deleted = false;

                if (self->m_mode == SyncMode::OneWay) {
                    // 单向模式：强制递归删除
                    if (std::filesystem::remove_all(full_path, ec) != static_cast<std::uintmax_t>(-1)) {
                        deleted = true;
                    }
                } else {
                    // 双向模式：只删空目录，作为保护机制
                    if (std::filesystem::remove(full_path, ec)) {
                        deleted = true;
                    } else if (ec && ec != std::errc::directory_not_empty) {
                        g_logger->warn("[Sync] 删除目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                    }
                }

                // 只有物理删除成功了，才从内存 Map 中移除
                if (deleted || (!deleted && !std::filesystem::exists(full_path))) {
                    self->m_state_manager->remove_dir_from_map(dir_path_str);
                }
            }
        }

        // E3. 创建缺失目录
        if (!dir_actions.dirs_to_create.empty()) {
            for (const auto& dir_path_str : dir_actions.dirs_to_create) {
                std::filesystem::path full_path = self->m_state_manager->get_root_path() / Utf8ToPath(dir_path_str);
                std::error_code ec;
                std::filesystem::create_directories(full_path, ec);
                if (!ec) {
                    self->m_state_manager->add_dir_to_map(dir_path_str);
                } else {
                    g_logger->warn("[Sync] 创建目录失败: {} | {}", dir_path_str, FormatErrorCode(ec));
                }
            }
        }

        // F. 发送文件请求 (涉及 KCP 发送，必须切回 IO 线程)
        if (!file_actions.files_to_request.empty()) {
            g_logger->info("[KCP] 计划向 {} 请求 {} 个缺失/过期的文件。", peer_id,
                           file_actions.files_to_request.size());

            boost::asio::post(self->m_io_context, [self, peer_id, reqs = std::move(file_actions.files_to_request)]() {
                std::lock_guard<std::mutex> lock(self->m_peers_mutex);
                auto it = self->m_peers_by_id.find(peer_id);
                if (it == self->m_peers_by_id.end()) return;

                auto peer_ctx = it->second.get();  // 获取裸指针用于发送
                for (const auto& file_path : reqs) {
                    nlohmann::json request_msg;
                    request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                    request_msg[Protocol::MSG_PAYLOAD] = {{"path", file_path}};
                    self->send_over_kcp_peer(request_msg.dump(), peer_ctx);
                }
            });
        }
    });
}

    void P2PManager::handle_file_update(const nlohmann::json& payload, PeerContext* from_peer) {
        if (m_role == SyncRole::Source && m_mode != SyncMode::BiDirectional) return;

        // 【同步会话】增加接收计数并刷新超时
        if (from_peer) {
            from_peer->received_file_count.fetch_add(1);
            
            // --- 刷新超时定时器 (在 IO 线程执行) ---
            std::string pid = from_peer->peer_id;
            uint64_t sid = from_peer->sync_session_id.load();
            
            boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
                 std::lock_guard<std::mutex> lock(m_peers_mutex);
                 auto it = m_peers_by_id.find(pid);
                 if (it != m_peers_by_id.end()) {
                     auto peer = it->second;
                     // 只有在 session 匹配且 timer 存在时才延期 
                     if (peer->sync_session_id.load() == sid && peer->sync_timeout_timer) {
                         peer->sync_timeout_timer->expires_after(std::chrono::seconds(60));
                         peer->sync_timeout_timer->async_wait([this, self, pid, sid, t=peer->sync_timeout_timer](const boost::system::error_code& ec){
                             if (ec) return; // 取消或被新的 wait 覆盖
                             
                             // 真正的超时检查
                             std::shared_ptr<PeerContext> p;
                             { std::lock_guard<std::mutex> l(m_peers_mutex); auto i=m_peers_by_id.find(pid); if(i!=m_peers_by_id.end()) p=i->second; }
                             
                             if(!p || p->sync_session_id != sid || p->sync_timeout_timer != t) return;

                             size_t rf = p->received_file_count.load(); size_t ef = p->expected_file_count.load();
                             size_t rd = p->received_dir_count.load(); size_t ed = p->expected_dir_count.load();

                             if (rf < ef || rd < ed) {
                                  g_logger->warn("[Sync] 滑动超时触发！进度 stalled wait at {}/{} files. Resending ACK.", rf, ef);
                                  send_sync_ack(p.get(), sid, rf, rd);
                             }
                         });
                     }
                 }
            });
        }

        FileInfo remote_info;
        try {
            remote_info = payload.get<FileInfo>();
        } catch (const std::exception& e) {
            g_logger->error("[KCP] 解析 file_update 失败: {}", e.what());
            return;
        }

        std::string peer_id = from_peer ? from_peer->peer_id : "";
        if (peer_id.empty()) return;

        // Offload 耗时操作到 Worker 线程
        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), remote_info, peer_id]() { // Capture remote_info by value
            //  --- 1. 拦截回声 (Echo Check) ---
            if (m_state_manager->should_ignore_echo(peer_id, remote_info.path, remote_info.hash)) {
                return;
            }

            g_logger->info("[P2P] 收到更新请求: {}", remote_info.path);

            std::filesystem::path relative_path(std::u8string_view(reinterpret_cast<const char8_t*>(remote_info.path.c_str()), remote_info.path.length()));
            std::filesystem::path full_path = m_state_manager->get_root_path() / relative_path;

            bool should_request = false;
            std::error_code ec;

            //  --- 2. 冲突检测 (Conflict Resolution) ---
            if (!std::filesystem::exists(full_path, ec)) {
                // 情况 0: 本地没有该文件 -> 直接请求 (Create/New)
                g_logger->info("[Sync] 本地缺失，准备下载: {}", remote_info.path);
                should_request = true;
            } else {
                std::string remote_hash = remote_info.hash;
                // 【耗时操作】在 Worker 线程计算 Hash，不再阻塞网络！
                std::string local_hash = Hashing::CalculateSHA256(full_path);                       
                std::string base_hash = m_state_manager->get_base_hash(peer_id, remote_info.path); 

                if (local_hash == remote_hash) {
                    g_logger->info("[Sync] 内容一致，无需更新: {}", remote_info.path);
                    m_state_manager->record_sync_success(peer_id, remote_info.path, local_hash);
                    return;
                }

                if (base_hash.empty() || local_hash == base_hash) {
                    g_logger->info("[Sync] 正常更新 (Local==Base): {}", remote_info.path);
                    should_request = true;
                } else {
                    g_logger->warn("[Sync] ⚠️ 检测到冲突: {}", remote_info.path);
                    g_logger->warn("       Base: {}...", base_hash.substr(0, 6));
                    g_logger->warn("       Local: {}...", local_hash.substr(0, 6));
                    g_logger->warn("       Remote: {}...", remote_hash.substr(0, 6));

                    auto now = std::chrono::system_clock::now();
                    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

                    std::string filename = relative_path.stem().string();
                    std::string ext = relative_path.extension().string();
                    std::string conflict_name = filename + ".conflict." + std::to_string(timestamp) + ext;
                    std::filesystem::path conflict_path = full_path.parent_path() / conflict_name;

                    std::error_code ren_ec;
                    std::filesystem::rename(full_path, conflict_path, ren_ec);

                    if (!ren_ec) {
                        g_logger->warn("[Sync] ⚡ 本地冲突文件已重命名为: {}", conflict_path.filename().string());
                        should_request = true;
                    } else {
                        g_logger->error("[Sync] ❌ 冲突处理失败 (无法重命名): {} | {}", remote_info.path, FormatErrorCode(ren_ec));
                        return;
                    }
                }
            }

            if (should_request) {
                nlohmann::json request_msg;
                request_msg[Protocol::MSG_TYPE] = Protocol::TYPE_REQUEST_FILE;
                request_msg[Protocol::MSG_PAYLOAD] = {{"path", remote_info.path}};
                
                // 【注意】必须发回到 IO 线程发送 KCP，确保线程安全
                std::string msg_str = request_msg.dump();
                boost::asio::post(m_io_context, [self, peer_id, msg_str]() {
                     std::lock_guard<std::mutex> lock(self->m_peers_mutex);
                     auto it = self->m_peers_by_id.find(peer_id);
                     if (it != self->m_peers_by_id.end() && it->second->kcp) {
                         self->send_over_kcp_peer(msg_str, it->second.get());
                     }
                });
            }
        });
    }
    void P2PManager::handle_file_delete(const nlohmann::json& payload, PeerContext* from_peer) {
        if (m_role != SyncRole::Destination && m_mode != SyncMode::BiDirectional) return;

        // 【同步会话】增加计数并刷新超时
        if (from_peer) {
            from_peer->received_file_count.fetch_add(1);
            
            std::string pid = from_peer->peer_id;
            uint64_t sid = from_peer->sync_session_id.load();
            boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
                 std::lock_guard<std::mutex> lock(m_peers_mutex);
                 auto it = m_peers_by_id.find(pid);
                 if (it != m_peers_by_id.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                     it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
                 }
            });
        }

        // 直接扔到 Worker 线程，不阻塞网络
        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
            std::string relative_path_str;
            try {
                relative_path_str = payload.at("path").get<std::string>();
            } catch (const std::exception& e) {
                g_logger->error("[KCP] (Destination) 解析 file_delete 失败: {}", e.what());
                return;
            }

            g_logger->info("[KCP] (Destination) 收到增量删除: {}", relative_path_str);

            std::filesystem::path relative_path(std::u8string_view(
                reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
            std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

            std::error_code ec;
            if (std::filesystem::remove(full_path, ec)) {
                g_logger->info("[Sync] -> 已删除本地文件: {}", relative_path_str);
                self->m_state_manager->remove_path_from_map(relative_path_str);
            } else {
                if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] ❌ 删除文件失败: {} | {}", relative_path_str, FormatErrorCode(ec));
                } else {
                    g_logger->info("[Sync] -> 本地文件已不存在, 无需操作: {}", relative_path_str);
                    self->m_state_manager->remove_path_from_map(relative_path_str);
                }
            }
        });
    }
    void P2PManager::handle_dir_create(const nlohmann::json& payload, PeerContext* from_peer) {
        if (m_role != SyncRole::Destination && m_mode != SyncMode::BiDirectional) return;

        // 【同步会话】增加接收计数并刷新超时
        if (from_peer) {
            from_peer->received_dir_count.fetch_add(1);

            std::string pid = from_peer->peer_id;
            uint64_t sid = from_peer->sync_session_id.load();
            boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
                 std::lock_guard<std::mutex> lock(m_peers_mutex);
                 auto it = m_peers_by_id.find(pid);
                 if (it != m_peers_by_id.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                     it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
                 }
            });
        }

        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
            std::string relative_path_str;
            try {
                relative_path_str = payload.at("path").get<std::string>();
            } catch (...) {
                return;
            }

            g_logger->info("[KCP] (Destination) 收到增量目录创建: {}", relative_path_str);

            std::filesystem::path relative_path(std::u8string_view(
                reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
            std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

            std::error_code ec;
            if (std::filesystem::create_directories(full_path, ec)) {
                g_logger->info("[Sync] -> 已创建目录: {}", relative_path_str);
                self->m_state_manager->add_dir_to_map(relative_path_str);
            } else if (ec) {
                g_logger->error("[Sync] ❌ 创建目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
            }
        });
    }
void P2PManager::handle_file_request(const nlohmann::json& payload, PeerContext* from_peer) {
    // 此函数现在可能不再被直接调用，或者仅仅作为 wrapper
    if (from_peer) {
        m_transfer_manager->queue_upload(from_peer->peer_id, payload);
    }
}
    void P2PManager::handle_dir_delete(const nlohmann::json& payload, PeerContext* from_peer) {
        if (m_role != SyncRole::Destination && m_mode != SyncMode::BiDirectional) return;

        // 【同步会话】刷新超时
        if (from_peer) {
            from_peer->received_dir_count.fetch_add(1);

            std::string pid = from_peer->peer_id;
            uint64_t sid = from_peer->sync_session_id.load();
            boost::asio::post(m_io_context, [this, self=shared_from_this(), pid, sid](){
                 std::lock_guard<std::mutex> lock(m_peers_mutex);
                 auto it = m_peers_by_id.find(pid);
                 if (it != m_peers_by_id.end() && it->second->sync_session_id.load() == sid && it->second->sync_timeout_timer) {
                     it->second->sync_timeout_timer->expires_after(std::chrono::seconds(60));
                 }
            });
        }

        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), payload]() {
            std::string relative_path_str;
            try {
                relative_path_str = payload.at("path").get<std::string>();
            } catch (...) {
                return;
            }

            g_logger->info("[KCP] (Destination) 收到增量目录删除: {}", relative_path_str);

            std::filesystem::path relative_path(std::u8string_view(
                reinterpret_cast<const char8_t*>(relative_path_str.c_str()), relative_path_str.length()));
            std::filesystem::path full_path = self->m_state_manager->get_root_path() / relative_path;

            std::error_code ec;
            std::filesystem::remove_all(full_path, ec);

            if (!ec) {
                g_logger->info("[Sync] -> 已删除目录: {}", relative_path_str);
                self->m_state_manager->remove_dir_from_map(relative_path_str);
            } else {
                if (ec != std::errc::no_such_file_or_directory) {
                    g_logger->error("[Sync] ❌ 删除目录失败: {} | {}", relative_path_str, FormatErrorCode(ec));
                } else {
                    self->m_state_manager->remove_dir_from_map(relative_path_str);
                }
            }
        });
    }

// --- 清理停滞的文件组装缓冲区 ---
void P2PManager::schedule_cleanup_task() {
    m_cleanup_timer.expires_after(std::chrono::minutes(5));  // 每5分钟执行一次
    m_cleanup_timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (!ec) {
            self->cleanup_stale_buffers();
            self->schedule_cleanup_task();  // 重新调度
        }
    });
}
std::vector<TransferStatus> P2PManager::get_active_transfers() {
    if (m_transfer_manager) {
        return m_transfer_manager->get_active_transfers();
    }
    return {};
}
void P2PManager::cleanup_stale_buffers() {
    if (m_transfer_manager) {
        m_transfer_manager->cleanup_stale_buffers();
    }
}
// --- UPnP 功能实现 ---

void P2PManager::init_upnp() {
    // 在 P2P 的 IO 线程中执行,避免阻塞
    boost::asio::post(m_worker_pool, [this]() {
        int error = 0;
        // 发现路由器 (2000ms 超时)
        struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        std::lock_guard<std::mutex> lock(m_upnp_mutex);
        if (devlist) {
            g_logger->info("[UPnP] 发现 UPnP 设备列表。");
            // 获取有效的 IGD (互联网网关设备)
            // API: UPNP_GetValidIGD(devlist, urls, data, lanaddr, lanaddrlen, wanaddr, wanaddrlen)
            char wanaddr[64] = {0};
            int r = UPNP_GetValidIGD(devlist, &m_upnp_urls, &m_upnp_data, m_upnp_lan_addr, sizeof(m_upnp_lan_addr),
                                     wanaddr, sizeof(wanaddr));

            if (r == 1) {
                g_logger->info("[UPnP] 成功连接到路由器: {}", m_upnp_urls.controlURL);
                g_logger->info("[UPnP] 我们的局域网 IP: {}", m_upnp_lan_addr);

                // 获取公网 IP
                char public_ip[40];
                r = UPNP_GetExternalIPAddress(m_upnp_urls.controlURL, m_upnp_data.first.servicetype, public_ip);

                if (r == UPNPCOMMAND_SUCCESS) {
                    m_upnp_public_ip = public_ip;
                    m_upnp_available = true;
                    g_logger->info("[UPnP] 成功获取公网 IP: {}", m_upnp_public_ip);
                } else {
                    g_logger->warn("[UPnP] 无法获取公网 IP (错误码: {}).", r);
                }
            } else {
                g_logger->warn("[UPnP] 未找到有效的 IGD (互联网网关设备).");
            }
            freeUPNPDevlist(devlist);
        } else {
            g_logger->warn("[UPnP] 未发现 UPnP 设备 (错误: {}).", error);
        }
    });
}

// 辅助函数，用于解析 SDP
static std::string get_sdp_field(const std::string& sdp, int index) {
    std::istringstream iss(sdp);
    std::string field;
    for (int i = 0; i <= index; ++i) {
        iss >> field;
        if (iss.fail()) return "";
    }
    return field;
}

std::string P2PManager::rewrite_candidate(const std::string& sdp_candidate) {
    std::lock_guard<std::mutex> lock(m_upnp_mutex);

    // 如果 UPnP 不可用，或者我们没有公网IP，则不重写
    if (!m_upnp_available || m_upnp_public_ip.empty()) {
        return sdp_candidate;
    }

    // libjuice 的候选地址格式: "a=candidate:..."
    // 我们只关心 "host" 类型的候选地址，它们包含局域网IP
    std::string cand_type = get_sdp_field(sdp_candidate, 7);
    if (cand_type != "host") {
        return sdp_candidate;  // 不是 "host"，可能是 "srflx" 或 "relay"，直接返回
    }

    // "a=candidate:..." 字段: 4=ip, 5=port
    std::string local_ip = get_sdp_field(sdp_candidate, 4);
    std::string local_port = get_sdp_field(sdp_candidate, 5);

    // 确保是我们自己的局域网 IP
    if (local_ip != m_upnp_lan_addr) {
        g_logger->debug("[UPnP] 候选 IP {} 与 UPnP 局域网 IP {} 不匹配，跳过。", local_ip, m_upnp_lan_addr);
        return sdp_candidate;
    }

    // 尝试在路由器上添加这个端口映射
    // (将 公网端口 映射到 局域网IP:局域网端口)
    int r = UPNP_AddPortMapping(m_upnp_urls.controlURL, m_upnp_data.first.servicetype,
                                local_port.c_str(),  // external_port (使用与内部相同的端口)
                                local_port.c_str(),  // internal_port
                                m_upnp_lan_addr,     // internal_client
                                "VeritasSync P2P",   // description
                                "UDP",               // protocol
                                nullptr, "0");       // remote_host, duration

    if (r == UPNPCOMMAND_SUCCESS) {
        g_logger->info("[UPnP] 成功为候选地址 {}:{} 映射公网端口 {}", local_ip, local_port, local_port);

        // 成功！现在重写候选地址，用公网IP替换局域网IP
        std::string rewritten_candidate = sdp_candidate;
        size_t pos = rewritten_candidate.find(local_ip);
        if (pos != std::string::npos) {
            rewritten_candidate.replace(pos, local_ip.length(), m_upnp_public_ip);
            g_logger->info("[UPnP] 重写候选地址为: {}...", rewritten_candidate.substr(0, 40));
            return rewritten_candidate;
        }
    } else {
        g_logger->warn("[UPnP] 无法为 {}:{} 映射端口 (错误码: {}).", local_ip, local_port, r);
    }

    // 映射失败，返回原始候选地址
    return sdp_candidate;
}

// ============================================================
// 【 1.2 版本】可靠同步会话机制
// ============================================================

void P2PManager::send_sync_begin(PeerContext* peer, uint64_t session_id, size_t file_count, size_t dir_count) {
    if (!peer || !peer->is_valid.load() || !peer->kcp) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_BEGIN;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"file_count", file_count},
        {"dir_count", dir_count}
    };
    send_over_kcp_peer(msg.dump(), peer);
}

void P2PManager::send_sync_ack(PeerContext* peer, uint64_t session_id, size_t received_files, size_t received_dirs) {
    if (!peer || !peer->is_valid.load() || !peer->kcp) return;
    
    nlohmann::json msg;
    msg[Protocol::MSG_TYPE] = Protocol::TYPE_SYNC_ACK;
    msg[Protocol::MSG_PAYLOAD] = {
        {"session_id", session_id},
        {"received_files", received_files},
        {"received_dirs", received_dirs}
    };
    send_over_kcp_peer(msg.dump(), peer);
}

void P2PManager::handle_sync_begin(const nlohmann::json& payload, PeerContext* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t session_id = payload.at("session_id").get<uint64_t>();
        size_t file_count = payload.at("file_count").get<size_t>();
        size_t dir_count = payload.at("dir_count").get<size_t>();
        
        g_logger->info("[Sync] 收到同步开始通知 (session: {}, 预期: {} 文件, {} 目录)",
                       session_id, file_count, dir_count);
        
        // 记录预期数量
        from_peer->sync_session_id.store(session_id);
        from_peer->expected_file_count.store(file_count);
        from_peer->expected_dir_count.store(dir_count);
        from_peer->received_file_count.store(0);
        from_peer->received_dir_count.store(0);
        
        // 启动智能定时器 (初始 60s)
        from_peer->sync_timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
        auto timer = from_peer->sync_timeout_timer;
        timer->expires_after(std::chrono::seconds(60));

        std::string pid = from_peer->peer_id; // Copy ID
        
        timer->async_wait([this, self = shared_from_this(), pid, 
                           session_id, file_count, dir_count, timer](const boost::system::error_code& ec) {
            if (ec) return;
            
            std::shared_ptr<PeerContext> peer;
            {
                std::lock_guard<std::mutex> lock(m_peers_mutex);
                auto it = m_peers_by_id.find(pid);
                if (it == m_peers_by_id.end()) return;
                peer = it->second;
            }
            
            // 检查是否是同一个 session 且是同一个 timer
            if (peer->sync_session_id.load() != session_id) return;
            if (peer->sync_timeout_timer != timer) return;
            
            size_t received_files = peer->received_file_count.load();
            size_t received_dirs = peer->received_dir_count.load();
            
            if (received_files < file_count || received_dirs < dir_count) {
                g_logger->warn("[Sync] 同步不完整 (超时)！收到 {}/{} 文件, {}/{} 目录。发送 ACK 请求补发...",
                               received_files, file_count, received_dirs, dir_count);
                send_sync_ack(peer.get(), session_id, received_files, received_dirs);
            } else {
                g_logger->info("[Sync] 同步完成！收到全部 {} 文件和 {} 目录", file_count, dir_count);
            }
        });
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 解析 sync_begin 失败: {}", e.what());
    }
}

void P2PManager::handle_sync_ack(const nlohmann::json& payload, PeerContext* from_peer) {
    if (!from_peer) return;
    
    try {
        uint64_t ack_session_id = payload.at("session_id").get<uint64_t>();
        size_t received_files = payload.at("received_files").get<size_t>();
        size_t received_dirs = payload.at("received_dirs").get<size_t>();
        
        // 【关键】检查这是否针对当前活跃会话的回复
        uint64_t current_id = from_peer->sync_session_id.load();
        if (ack_session_id != current_id) {
            g_logger->warn("[Sync] 收到过时或不匹配的 ACK (ACK ID: {}, 当前 ID: {})，忽略。", ack_session_id, current_id);
            return;
        }

        g_logger->warn("[Sync] 收到 ACK (ID: {}): 对方只收到 {} 文件, {} 目录。重新同步...",
                       ack_session_id, received_files, received_dirs);
        
        // 既然对方没收完，且会话 ID 匹配，说明确实需要补发
        // 我们生成一个新的 ID 并重新开始推送
        uint64_t new_session_id = std::chrono::steady_clock::now().time_since_epoch().count();
        from_peer->sync_session_id.store(new_session_id);
        
        boost::asio::post(m_worker_pool, [this, self = shared_from_this(), 
                                          ctx = std::shared_ptr<PeerContext>(from_peer->p2p_manager_ptr, from_peer), 
                                          new_session_id]() {
            // 重新获取 peer 的 shared_ptr
            std::shared_ptr<PeerContext> peer;
            {
                std::lock_guard<std::mutex> lock(m_peers_mutex);
                auto it = m_peers_by_id.find(ctx->peer_id);
                if (it != m_peers_by_id.end()) {
                    peer = it->second;
                }
            }
            if (peer) {
                perform_flood_sync(peer, new_session_id);
            }
        });
    } catch (const std::exception& e) {
        g_logger->error("[Sync] 解析 sync_ack 失败: {}", e.what());
    }
}

void P2PManager::perform_flood_sync(std::shared_ptr<PeerContext> ctx, uint64_t session_id) {
    if (!ctx || !ctx->is_valid.load() || !m_state_manager) {
        g_logger->warn("[Sync] perform_flood_sync: 上下文无效，跳过");
        return;
    }
    
    // 检查 session_id 是否一致（防止重复执行旧会话）
    if (ctx->sync_session_id.load() != session_id) {
        g_logger->info("[Sync] 会话 ID 已变更，跳过本次同步");
        return;
    }
    
    // 1. 扫描目录获取所有文件
    m_state_manager->scan_directory();
    std::vector<FileInfo> files = m_state_manager->get_all_files();
    std::set<std::string> dirs = m_state_manager->get_local_directories();

    if (files.empty() && dirs.empty()) {
        g_logger->info("[P2P] 没有文件需要推送给 {}", ctx->peer_id);
        return;
    }

    g_logger->info("[P2P] 开始向 {} 推送 {} 个文件和 {} 个目录 (session: {})...", 
                   ctx->peer_id, files.size(), dirs.size(), session_id);

    // 2. 【关键】先发送 sync_begin 通知对方预期数量
    boost::asio::post(m_io_context, [this, self = shared_from_this(), ctx, session_id, 
                                     file_count = files.size(), dir_count = dirs.size()]() {
        send_sync_begin(ctx.get(), session_id, file_count, dir_count);
    });

    // 3. 发送目录信息
    for (const auto& dir_path : dirs) {
        // 【关键】检查连接有效性
        if (!ctx->is_valid.load()) {
            g_logger->warn("[P2P] 连接已断开，停止发送目录 (session: {})", session_id);
            return;
        }
        
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_DIR_CREATE;
        msg[Protocol::MSG_PAYLOAD] = {{"path", dir_path}};

        std::weak_ptr<PeerContext> weak_ctx = ctx;
        boost::asio::post(m_io_context, [self = shared_from_this(), weak_ctx, msg_str = msg.dump()]() {
            auto ctx_locked = weak_ctx.lock();
            if (ctx_locked && ctx_locked->is_valid.load() && ctx_locked->kcp) {
                self->send_over_kcp_peer(msg_str, ctx_locked.get());
            }
        });
    }

    // 4. 逐个发送文件状态
    for (size_t i = 0; i < files.size(); ++i) {
        // 【关键】检查连接有效性和会话有效性
        if (!ctx->is_valid.load()) {
            g_logger->warn("[P2P] 连接已断开，停止发送文件 (session: {}, 已发送 {}/{})", 
                           session_id, i, files.size());
            return;
        }
        
        if (ctx->sync_session_id.load() != session_id) {
            g_logger->info("[Sync] 会话 ID 已变更，停止本次同步 (已发送 {}/{})", i, files.size());
            return;
        }
        
        const auto& file = files[i];
        nlohmann::json msg;
        msg[Protocol::MSG_TYPE] = Protocol::TYPE_FILE_UPDATE;
        msg[Protocol::MSG_PAYLOAD] = file;

        std::weak_ptr<PeerContext> weak_ctx = ctx;
        boost::asio::post(m_io_context, [self = shared_from_this(), weak_ctx, msg_str = msg.dump()]() {
            auto ctx_locked = weak_ctx.lock();
            if (ctx_locked && ctx_locked->is_valid.load() && ctx_locked->kcp) {
                self->send_over_kcp_peer(msg_str, ctx_locked.get());
            }
        });

        // 【流控】每 50 个文件检查一次 KCP 发送队列积压量
        if ((i + 1) % 50 == 0) {
            int pending = 0;
            {
                std::lock_guard<std::mutex> lock(m_peers_mutex);
                if (ctx->is_valid.load() && ctx->kcp) {
                    pending = ikcp_waitsnd(ctx->kcp);
                }
            }

            while (pending > 1024 && ctx->is_valid.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                {
                    std::lock_guard<std::mutex> lock(m_peers_mutex);
                    pending = ctx->kcp ? ikcp_waitsnd(ctx->kcp) : 0;
                }
            }
        }
    }

    g_logger->info("[P2P] 向 {} 推送文件状态完成 ({} 个文件, {} 个目录, session: {})", 
                   ctx->peer_id, files.size(), dirs.size(), session_id);
}

}  // namespace VeritasSync
