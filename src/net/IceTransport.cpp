#include "VeritasSync/net/IceTransport.h"
#include "VeritasSync/common/Logger.h"

#include <cstring>

namespace VeritasSync {

// --- 工厂方法 ---
std::shared_ptr<IceTransport> IceTransport::create(
    const IceConfig& config,
    IceTransportCallbacks callbacks) {
    
    // 使用 make_shared 的技巧：通过派生类访问私有构造函数
    struct IceTransportMaker : public IceTransport {
        IceTransportMaker(IceTransportCallbacks cb) : IceTransport(std::move(cb)) {}
    };
    
    auto transport = std::make_shared<IceTransportMaker>(std::move(callbacks));
    if (!transport->initialize(config)) {
        return nullptr;
    }
    return transport;
}

// --- 构造与析构 ---
IceTransport::IceTransport(IceTransportCallbacks callbacks)
    : m_callbacks(std::move(callbacks)) {
}

IceTransport::~IceTransport() {
    if (m_agent) {
        juice_destroy(m_agent);
        m_agent = nullptr;
    }
}

bool IceTransport::initialize(const IceConfig& config) {
    juice_config_t jconfig = {};
    
    // 配置 STUN
    if (!config.stun_host.empty()) {
        jconfig.stun_server_host = config.stun_host.c_str();
        jconfig.stun_server_port = config.stun_port;
    }
    
    // TURN 配置需要静态存储（libjuice 的限制）
    static juice_turn_server_t turn_server = {};
    if (!config.turn_host.empty()) {
        turn_server.host = config.turn_host.c_str();
        turn_server.port = config.turn_port;
        turn_server.username = config.turn_username.c_str();
        turn_server.password = config.turn_password.c_str();
        jconfig.turn_servers = &turn_server;
        jconfig.turn_servers_count = 1;
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
    if (m_agent) {
        m_state.store(IceState::Gathering);
        juice_gather_candidates(m_agent);
    }
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
    
    char buffer[4096];
    if (juice_get_local_description(m_agent, buffer, sizeof(buffer)) == 0) {
        return std::string(buffer);
    }
    return "";
}

bool IceTransport::get_selected_candidates(std::string& out_local, std::string& out_remote) const {
    if (!m_agent) return false;
    
    char local_buf[1024];
    char remote_buf[1024];
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
void IceTransport::on_juice_state_changed(juice_agent_t* /*agent*/, juice_state_t state, void* user_ptr) {
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
    if (self && self->m_callbacks.on_gathering_done) {
        std::string local_desc = self->get_local_description();
        self->m_callbacks.on_gathering_done(local_desc);
    }
}

// 对应 IceTransportCallbacks::on_data_received
void IceTransport::on_juice_recv(juice_agent_t* /*agent*/, const char* data, size_t size, void* user_ptr) {
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
