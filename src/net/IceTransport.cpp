#include "VeritasSync/net/IceTransport.h"
#include "VeritasSync/net/StunProber.h"
#include "VeritasSync/common/Logger.h"

#include <juice/juice.h>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstring>
#include <mutex>

namespace VeritasSync {

// --- libjuice 日志集成（只初始化一次）---
static std::once_flag s_juice_log_init;

static void init_juice_logging() {
    std::call_once(s_juice_log_init, []() {
        juice_set_log_level(JUICE_LOG_LEVEL_INFO);
        juice_set_log_handler([](juice_log_level_t level, const char* message) {
            if (!g_logger) return;
            // 去除 libjuice 日志末尾的换行符
            std::string msg(message);
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();

            switch (level) {
                case JUICE_LOG_LEVEL_VERBOSE:
                case JUICE_LOG_LEVEL_DEBUG:
                    g_logger->debug("[libjuice] {}", msg);
                    break;
                case JUICE_LOG_LEVEL_INFO:
                    g_logger->info("[libjuice] {}", msg);
                    break;
                case JUICE_LOG_LEVEL_WARN:
                    g_logger->warn("[libjuice] {}", msg);
                    break;
                default:
                    g_logger->error("[libjuice] {}", msg);
                    break;
            }
        });
    });
}

// E-1: 魔数统一为命名常量
static constexpr size_t SDP_BUFFER_SIZE       = 4096;  // SDP 描述缓冲区大小
static constexpr size_t CANDIDATE_BUFFER_SIZE = 1024;  // ICE 候选地址缓冲区大小

// --- 工厂方法 ---
std::shared_ptr<IceTransport> IceTransport::create(
    const IceConfig& config,
    IceTransportCallbacks callbacks,
    boost::asio::io_context* io_context) {

    // 使用 make_shared 的技巧：通过派生类访问私有构造函数
    struct IceTransportMaker : public IceTransport {
        IceTransportMaker(IceTransportCallbacks cb, boost::asio::io_context* io)
            : IceTransport(std::move(cb), io) {}
    };

    auto transport = std::make_shared<IceTransportMaker>(std::move(callbacks), io_context);
    if (!transport->initialize(config)) {
        return nullptr;
    }
    return transport;
}

// --- 构造与析构 ---
IceTransport::IceTransport(IceTransportCallbacks callbacks, boost::asio::io_context* io_context)
    : m_callbacks(std::move(callbacks)), m_io_context(io_context) {
}

IceTransport::~IceTransport() {
    if (m_agent) {
        juice_destroy(m_agent);
        m_agent = nullptr;
    }
}

bool IceTransport::initialize(const IceConfig& config) {
    init_juice_logging();  // 确保 libjuice 日志已初始化
    juice_config_t jconfig = {};

    // 【安全】将配置字符串拷贝到成员变量，确保 c_str() 在 agent 生命周期内有效
    m_stun_host = config.stun_host;
    m_turn_host = config.turn_host;
    m_turn_username = config.turn_username;
    m_turn_password = config.turn_password;

    // 保存 Multi-STUN 配置
    m_extra_stun_servers = config.extra_stun_servers;
    m_enable_multi_stun_probing = config.enable_multi_stun_probing;
    m_multi_stun_hold_timeout = std::chrono::milliseconds(config.multi_stun_hold_timeout_ms);
    
    // 配置 STUN
    if (!m_stun_host.empty()) {
        jconfig.stun_server_host = m_stun_host.c_str();
        jconfig.stun_server_port = config.stun_port;
    }
    
    // 【关键修复】TURN 配置使用成员变量 m_turn_server，避免局部变量导致悬空指针
    // 原代码使用局部变量 turn_server，函数返回后jconfig.turn_servers变成悬空指针
    if (!m_turn_host.empty()) {
        m_turn_server.host = m_turn_host.c_str();
        m_turn_server.port = config.turn_port;
        m_turn_server.username = m_turn_username.c_str();
        m_turn_server.password = m_turn_password.c_str();
        jconfig.turn_servers = &m_turn_server;  // 指向成员变量，生命周期与IceTransport相同
        jconfig.turn_servers_count = 1;
    } else {
        jconfig.turn_servers = nullptr;
        jconfig.turn_servers_count = 0;
    }
    
    // 设置回调
    jconfig.user_ptr = this;
    jconfig.cb_state_changed = &IceTransport::on_juice_state_changed;
    jconfig.cb_candidate = &IceTransport::on_juice_candidate;
    jconfig.cb_gathering_done = &IceTransport::on_juice_gathering_done;
    jconfig.cb_recv = &IceTransport::on_juice_recv;
    
    m_agent = juice_create(&jconfig);
    if (!m_agent) {
        if (g_logger) g_logger->error("[IceTransport] juice_create failed");
        return false;
    }
    
    return true;
}

/*

客户端 A                      信令服务器                    客户端 B
   │                              │                            │
   ├─ gather_candidates() ────────┤                            │
   │   生成本地候选                │                            │
   ├─ on_local_candidate ─────────┤                            │
   │   × N 次                      │                            │
   ├─ on_gathering_done ──────────┼─ 转发 SDP ───────────────→│
   │                              │                            ├─ set_remote_description()
   │                              │                            ├─ add_remote_candidate() × N
   │                              │                            │
   │←──── 转发 SDP ───────────────┼─ on_gathering_done ───────┤
   ├─ set_remote_description()    │                            │
   ├─ add_remote_candidate() × N  │                            │
   │                              │                            │
   ├─ STUN 连接性检查 ←─────────────────────────────────────→│
   │   (P2P 直连尝试)              │                            │
   ├─ on_state_changed(Connected) │                            │
   │                              │                            │
*/
// --- 公共接口 ---
void IceTransport::gather_candidates() {
    if (!m_agent) return;

    m_state.store(IceState::Gathering);
    m_juice_gathering_done.store(false);
    m_multi_stun_done.store(false);
    m_gathering_emitted.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_multi_stun_results.clear();
    }

    // ★ 关键变更：与 libjuice gathering 同时启动 Multi-STUN
    if (m_enable_multi_stun_probing && !m_extra_stun_servers.empty() && m_io_context) {
        start_multi_stun_probing();  // 非阻塞，结果通过回调到达
    } else {
        m_multi_stun_done.store(true);  // 不需要探测，标记完成
    }

    juice_gather_candidates(m_agent);  // libjuice 开始自己的 gathering
}

void IceTransport::set_remote_description(const std::string& remote_sdp) {
    if (m_agent) {
        juice_set_remote_description(m_agent, remote_sdp.c_str());
    }
}

void IceTransport::add_remote_candidate(const std::string& candidate) {
    if (m_agent) {
        juice_add_remote_candidate(m_agent, candidate.c_str());
    }
}

void IceTransport::set_remote_gathering_done() {
    if (m_agent) {
        juice_set_remote_gathering_done(m_agent);
    }
}

std::string IceTransport::get_local_description() const {
    if (!m_agent) return "";
    
    char buffer[SDP_BUFFER_SIZE];
    if (juice_get_local_description(m_agent, buffer, sizeof(buffer)) == 0) {
        return std::string(buffer);
    }
    return "";
}

bool IceTransport::get_selected_candidates(std::string& out_local, std::string& out_remote) const {
    if (!m_agent) return false;
    
    char local_buf[CANDIDATE_BUFFER_SIZE];
    char remote_buf[CANDIDATE_BUFFER_SIZE];
    if (juice_get_selected_candidates(m_agent, local_buf, sizeof(local_buf), 
                                       remote_buf, sizeof(remote_buf)) == 0) {
        out_local = local_buf;
        out_remote = remote_buf;
        return true;
    }
    return false;
}

int IceTransport::send(const char* data, size_t size) {
    if (m_agent) {
        return juice_send(m_agent, data, size);
    }
    return -1;
}

bool IceTransport::is_connected() const {
    auto state = m_state.load();
    return state == IceState::Connected || state == IceState::Completed;
}

// --- 静态回调 (libjuice 线程) ---
// 这样的设计模式也叫 适配器模式 (Adapter Pattern)，将 libjuice 的 C 风格回调适配为 C++ 的 std::function 回调，让上层代码更加现代和安全！

// 对应 IceTransportCallbacks::on_state_changed
void IceTransport::on_juice_state_changed([[maybe_unused]] juice_agent_t* agent, juice_state_t state, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (self) {
        self->handle_state_changed(state);
    }
}

// 对应 IceTransportCallbacks::on_local_candidate
void IceTransport::on_juice_candidate(juice_agent_t* /*agent*/, const char* sdp, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (self && self->m_callbacks.on_local_candidate && sdp) {
        std::string candidate(sdp);
        // 移除尾部换行
        while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n')) {
            candidate.pop_back();
        }
        self->m_callbacks.on_local_candidate(candidate);
    }
}

// 对应 IceTransportCallbacks::on_gathering_done
void IceTransport::on_juice_gathering_done(juice_agent_t* /*agent*/, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (!self) return;

    self->m_juice_gathering_done.store(true);

    // ★ 尝试汇合：如果 Multi-STUN 也完成了就立即发 SDP
    if (self->m_io_context) {
        boost::asio::post(*self->m_io_context, [self]() {
            self->try_finalize_gathering();
        });
    } else {
        // 无 io_context，同步调用
        self->try_finalize_gathering();
    }
}

// 对应 IceTransportCallbacks::on_data_received
void IceTransport::on_juice_recv([[maybe_unused]] juice_agent_t* agent, const char* data, size_t size, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (self && self->m_callbacks.on_data_received) {
        self->m_callbacks.on_data_received(data, size);
    }
}

// --- 内部处理 ---
void IceTransport::handle_state_changed(juice_state_t state) {
    IceState new_state = IceState::New;
    
    switch (state) {
        case JUICE_STATE_DISCONNECTED:
            new_state = IceState::Disconnected;
            break;
        case JUICE_STATE_GATHERING:
            new_state = IceState::Gathering;
            break;
        case JUICE_STATE_CONNECTING:
            new_state = IceState::Connecting;
            break;
        case JUICE_STATE_CONNECTED:
            new_state = IceState::Connected;
            update_connection_type();
            break;
        case JUICE_STATE_COMPLETED:
            new_state = IceState::Completed;
            update_connection_type();
            break;
        case JUICE_STATE_FAILED:
            new_state = IceState::Failed;
            break;
        default:
            break;
    }
    
    m_state.store(new_state);
    
    if (m_callbacks.on_state_changed) {
        m_callbacks.on_state_changed(new_state);
    }
}

void IceTransport::update_connection_type() {
    std::string local_candidate, remote_candidate;
    if (get_selected_candidates(local_candidate, remote_candidate)) {
        if (local_candidate.find(" typ host") != std::string::npos ||
            local_candidate.find(" typ srflx") != std::string::npos) {
            m_connection_type.store(IceConnectionType::Direct);
        } else if (local_candidate.find(" typ relay") != std::string::npos) {
            m_connection_type.store(IceConnectionType::Relay);
        }
    }
}

// --- Multi-STUN Probing ---

void IceTransport::start_multi_stun_probing() {
    if (!m_enable_multi_stun_probing || m_extra_stun_servers.empty() || !m_io_context) {
        m_multi_stun_done.store(true);
        return;
    }

    if (g_logger) {
        g_logger->info("[IceTransport] 启动 Multi-STUN 并行探测，{} 个额外服务器",
                       m_extra_stun_servers.size());
    }

    auto servers = m_extra_stun_servers;  // 拷贝，避免在异步回调中引用 this

    // 使用调用方的 io_context（由 PeerController 传入），无需创建临时线程
    auto prober = std::make_shared<StunProber>(*m_io_context);

    prober->probe(servers, std::chrono::milliseconds(3000),
        [self_weak = weak_from_this(), prober](std::vector<StunResult> results) {
            auto self = self_weak.lock();
            if (!self) return;

            if (g_logger) {
                g_logger->info("[IceTransport] Multi-STUN 探测完成，发现 {} 个 reflexive 地址",
                               results.size());
            }

            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                self->m_multi_stun_results = std::move(results);
            }
            self->m_multi_stun_done.store(true);

            // 触发汇合
            if (self->m_io_context) {
                boost::asio::post(*self->m_io_context, [self]() {
                    self->try_finalize_gathering();
                });
            }
        });
}

// --- 汇合逻辑 ---

void IceTransport::try_finalize_gathering() {
    if (!m_juice_gathering_done.load()) return;  // libjuice 还没完成

    if (m_multi_stun_done.load()) {
        // 两者都完成 → 取消 hold timer，发送 SDP
        if (m_stun_hold_timer) {
            m_stun_hold_timer->cancel();
            m_stun_hold_timer.reset();
        }
        emit_sdp_with_extra_candidates();
        return;
    }

    // libjuice 完成但 Multi-STUN 未完成 → 启动 hold timer（仅一次）
    if (!m_stun_hold_timer) {
        m_stun_hold_timer = std::make_shared<boost::asio::steady_timer>(*m_io_context);
        m_stun_hold_timer->expires_after(m_multi_stun_hold_timeout);
        m_stun_hold_timer->async_wait([self_weak = weak_from_this()](const boost::system::error_code& ec) {
            if (ec) return;
            auto self = self_weak.lock();
            if (!self) return;

            size_t result_count;
            {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                result_count = self->m_multi_stun_results.size();
            }

            if (g_logger) {
                g_logger->info("[IceTransport] Multi-STUN hold 超时 ({}ms)，使用已有 {} 个结果",
                              self->m_multi_stun_hold_timeout.count(), result_count);
            }
            self->m_multi_stun_done.store(true);
            self->emit_sdp_with_extra_candidates();
        });
    }
}

// --- SDP 嵌入额外候选 ---

uint16_t IceTransport::parse_port_from_candidate_line(const std::string& sdp, size_t srflx_pos) {
    // SDP candidate 格式: a=candidate:foundation component protocol priority IP PORT typ type ...
    // PORT 是第 6 个空格分隔字段（0-indexed = 5）
    // 先回到包含 "typ srflx" 的这一行的起始位置

    size_t line_start = sdp.rfind('\n', srflx_pos);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;

    // 跳过 "a=" 前缀（如果有）
    size_t pos = line_start;
    if (sdp.compare(pos, 2, "a=") == 0) {
        pos += 2;
    }

    // 按空格分割，找到第 6 个字段（index 5 = port）
    int field_index = 0;
    while (pos < sdp.size() && field_index < 5) {
        // 跳过当前字段
        while (pos < sdp.size() && sdp[pos] != ' ' && sdp[pos] != '\r' && sdp[pos] != '\n') ++pos;
        // 跳过空格
        while (pos < sdp.size() && sdp[pos] == ' ') ++pos;
        ++field_index;
    }

    if (field_index != 5 || pos >= sdp.size()) return 0;

    // 提取 port 字段
    size_t port_start = pos;
    while (pos < sdp.size() && sdp[pos] != ' ' && sdp[pos] != '\r' && sdp[pos] != '\n') ++pos;

    std::string port_str = sdp.substr(port_start, pos - port_start);
    try {
        int port = std::stoi(port_str);
        if (port > 0 && port <= 65535) return static_cast<uint16_t>(port);
    } catch (...) {}
    return 0;
}

void IceTransport::emit_sdp_with_extra_candidates() {
    // 防重入：hold timer 超时和 Multi-STUN 完成可能都触发此函数
    bool expected = false;
    if (!m_gathering_emitted.compare_exchange_strong(expected, true)) {
        if (g_logger) {
            g_logger->debug("[IceTransport] emit_sdp_with_extra_candidates 已执行过，跳过重复调用");
        }
        return;
    }

    std::string sdp = get_local_description();  // libjuice 原生 SDP

    // 【诊断】记录 SDP 中的候选者类型，帮助排查 NAT 穿透问题
    if (g_logger) {
        bool has_host = sdp.find("typ host") != std::string::npos;
        bool has_srflx = sdp.find("typ srflx") != std::string::npos;
        bool has_relay = sdp.find("typ relay") != std::string::npos;
        g_logger->info("[ICE] Gathering 完成。候选者类型: host={} srflx={} relay={}",
                       has_host ? "✓" : "✗", has_srflx ? "✓" : "✗", has_relay ? "✓" : "✗");
        if (!has_srflx && !has_relay) {
            g_logger->warn("[ICE] ⚠️ 无 srflx/relay 候选者！STUN 服务器可能不可达，NAT 穿透将失败");
        }
    }

    // 从 libjuice SDP 中提取主 STUN 发现的端口（用于构造替代候选）
    uint16_t juice_port = 0;
    auto srflx_pos = sdp.find("typ srflx");
    if (srflx_pos != std::string::npos) {
        juice_port = parse_port_from_candidate_line(sdp, srflx_pos);
    }

    // 构造额外候选行并追加到 SDP
    std::vector<StunResult> results;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        results = std::move(m_multi_stun_results);  // move 而非 copy
    }

    std::string extra_lines;
    extra_lines.reserve(results.size() * 200);  // 预分配，每个结果约 2 行 × ~100 字节
    for (const auto& r : results) {
        // 候选1：原始 IP:port（StunProber 的 socket 端口）
        extra_lines += "a=candidate:multi1 1 UDP 16777215 ";
        extra_lines += r.reflexive_ip;
        extra_lines += " ";
        extra_lines += std::to_string(r.reflexive_port);
        extra_lines += " typ srflx\r\n";

        // 候选2：探测 IP + libjuice 主端口（适配按IP分流的双WAN）
        if (juice_port > 0 && juice_port != r.reflexive_port) {
            extra_lines += "a=candidate:multi2 1 UDP 16777214 ";
            extra_lines += r.reflexive_ip;
            extra_lines += " ";
            extra_lines += std::to_string(juice_port);
            extra_lines += " typ srflx\r\n";
        }

        if (g_logger) {
            g_logger->info("[IceTransport] SDP 嵌入 Multi-STUN 候选: {}:{} (+ port {})",
                          r.reflexive_ip, r.reflexive_port, juice_port);
        }
    }

    if (!extra_lines.empty()) {
        sdp += extra_lines;  // 追加到 SDP 末尾
    }

    // 触发上层回调（PeerController → 发送 sdp_offer/sdp_answer）
    std::function<void(const std::string&)> cb_gathering;
    std::function<void()> cb_all_done;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb_gathering = m_callbacks.on_gathering_done;
        cb_all_done = m_callbacks.on_all_candidates_done;
    }

    if (cb_gathering) cb_gathering(sdp);
    if (cb_all_done) cb_all_done();  // 立即发 ice_gathering_done（候选已在 SDP 中）
}

} // namespace VeritasSync
