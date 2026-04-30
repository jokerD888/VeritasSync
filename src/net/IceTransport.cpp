#include "VeritasSync/net/IceTransport.h"
#include "VeritasSync/common/Logger.h"

#include <juice/juice.h>
#include <boost/asio/post.hpp>
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

// 命名常量
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

    // 将配置字符串拷贝到成员变量，确保 c_str() 在 agent 生命周期内有效
    m_stun_host = config.stun_host;
    m_turn_host = config.turn_host;
    m_turn_username = config.turn_username;
    m_turn_password = config.turn_password;

    // 配置 STUN
    if (!m_stun_host.empty()) {
        jconfig.stun_server_host = m_stun_host.c_str();
        jconfig.stun_server_port = config.stun_port;
    }
    
    // TURN 配置使用成员变量 m_turn_server，避免局部变量导致悬空指针
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
    juice_gather_candidates(m_agent);
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

    // 拷贝回调指针 + SDP，避免在持锁状态下调用上层回调
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        cb = self->m_callbacks.on_gathering_done;
    }
    if (!cb) return;

    std::string sdp = self->get_local_description();

    // 记录 SDP 中的候选者类型
    if (g_logger) {
        bool has_host  = sdp.find("typ host")  != std::string::npos;
        bool has_srflx = sdp.find("typ srflx") != std::string::npos;
        bool has_relay = sdp.find("typ relay") != std::string::npos;
        g_logger->info("[ICE] Gathering 完成。候选者类型: host={} srflx={} relay={}",
                       has_host  ? "✓" : "✗",
                       has_srflx ? "✓" : "✗",
                       has_relay ? "✓" : "✗");
        if (!has_srflx && !has_relay) {
            g_logger->warn("[ICE] ⚠️ 无 srflx/relay 候选者！STUN 服务器可能不可达，NAT 穿透将失败");
        }
    }

    // 在 io_context 上回调（如果有），否则同步触发
    if (self->m_io_context) {
        boost::asio::post(*self->m_io_context, [cb, sdp]() { cb(sdp); });
    } else {
        cb(sdp);
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

} // namespace VeritasSync
