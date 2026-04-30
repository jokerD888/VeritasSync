#include "VeritasSync/net/IceTransport.h"
#include "VeritasSync/common/Logger.h"

#include <juice/juice.h>
#include <boost/asio/post.hpp>
#include <cstring>
#include <mutex>  // std::once_flag / std::call_once（libjuice 日志一次性初始化）

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
static constexpr size_t SDP_BUFFER_SIZE       = 8192;  // SDP 描述缓冲区（候选多时 4KB 易截断）
static constexpr size_t CANDIDATE_BUFFER_SIZE = 1024;  // ICE 候选地址缓冲区大小

// --- 工厂方法 ---
std::shared_ptr<IceTransport> IceTransport::create(
    const IceConfig& config,
    IceTransportCallbacks callbacks,
    boost::asio::io_context& io_context) {

    // 使用 make_shared 的技巧：通过派生类访问私有构造函数
    struct IceTransportMaker : public IceTransport {
        IceTransportMaker(IceTransportCallbacks cb, boost::asio::io_context& io)
            : IceTransport(std::move(cb), io) {}
    };

    auto transport = std::make_shared<IceTransportMaker>(std::move(callbacks), io_context);
    if (!transport->initialize(config)) {
        return nullptr;
    }
    return transport;
}

// --- 构造与析构 ---
IceTransport::IceTransport(IceTransportCallbacks callbacks, boost::asio::io_context& io_context)
    : m_callbacks(std::move(callbacks)), m_io_context(io_context) {
}

IceTransport::~IceTransport() {
    // 注意：juice_destroy 会同步等待 libjuice polling 线程退出，
    // 因此在它返回之后，不会再有 on_juice_* 回调被触发。
    // 但 polling 线程在退出前可能已经把若干 lambda post 到 m_io_context，
    // 这些 lambda 用 weak_from_this() 锁定 self，本对象析构后 lock() 返回空，
    // 自然安全跳过 — 这是 P0 修复的核心保证。
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

        // 注意：当前 libjuice 仅支持 TURN over UDP（juice_turn_server_t 无 transport 字段，
        // 且上游 README 明确 "Only UDP is supported as transport protocol"）。
        // 若未来需要 TURN-TCP/TLS 兜底，需先升级 libjuice 并在此处补齐字段。

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
    if (juice_get_local_description(m_agent, buffer, sizeof(buffer)) != 0) {
        return "";
    }

    // libjuice 在缓冲不足时不报错、直接截断 → 检测末尾是否紧贴边界。
    // 用 strnlen 限定上限，避免 buffer 未以 \0 结尾时越界。
    size_t len = strnlen(buffer, sizeof(buffer));
    if (len >= sizeof(buffer) - 1 && g_logger) {
        g_logger->warn("[IceTransport] 本地 SDP 长度紧贴缓冲上限 ({} bytes)，可能已被截断；"
                       "若对端解析失败请考虑扩大 SDP_BUFFER_SIZE", len);
    }
    return std::string(buffer, len);
}

bool IceTransport::get_selected_candidates(std::string& out_local, std::string& out_remote) const {
    if (!m_agent) return false;

    // libjuice 在 ICE 未到 Connected 之前调用 juice_get_selected_candidates 会失败，
    // 这里前置过滤掉以避免无谓的 libjuice ERROR 日志，让调用方拿到清晰的"未连接"语义。
    if (!is_connected()) return false;

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

// --- 静态回调 (libjuice polling 线程) ---
//
// P0 修复：所有回调在 libjuice polling 线程上仅做"参数拷贝 + post 到 io_context"，
// 真正的业务处理在 io_context 线程内完成。这带来两个保证：
//   1. 回调线程一致性：上层 callbacks 全部在 io_context 线程上被调用，
//      上层无需再考虑线程切换、加锁。
//   2. 生命周期安全：post 时使用 weak_from_this()，若析构已发生，
//      lambda 在 io_context 上 lock() 返回空指针，自然安全跳过。
//
// 这种适配模式即 Adapter Pattern：把 libjuice 的 C 风格回调适配为
// std::function 风格的 C++ 接口。

void IceTransport::on_juice_state_changed([[maybe_unused]] juice_agent_t* agent,
                                          juice_state_t state, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (!self) return;
    auto weak = self->weak_from_this();
    boost::asio::post(self->m_io_context, [weak, state]() {
        if (auto sp = weak.lock()) {
            sp->handle_state_changed_on_loop(state);
        }
    });
}

void IceTransport::on_juice_candidate(juice_agent_t* /*agent*/, const char* sdp, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (!self || !sdp) return;

    // 立即拷贝 SDP（libjuice 回调返回后 sdp 指针不可信）
    std::string candidate(sdp);
    while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n')) {
        candidate.pop_back();
    }

    auto weak = self->weak_from_this();
    boost::asio::post(self->m_io_context,
                      [weak, candidate = std::move(candidate)]() {
        auto sp = weak.lock();
        if (!sp || !sp->m_callbacks.on_local_candidate) return;
        sp->m_callbacks.on_local_candidate(candidate);
    });
}

void IceTransport::on_juice_gathering_done(juice_agent_t* /*agent*/, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (!self) return;

    // 在 libjuice 线程上拿 SDP 是安全的（juice_get_local_description 是只读 API）
    std::string sdp = self->get_local_description();

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

    auto weak = self->weak_from_this();
    boost::asio::post(self->m_io_context, [weak, sdp = std::move(sdp)]() {
        auto sp = weak.lock();
        if (!sp || !sp->m_callbacks.on_gathering_done) return;
        sp->m_callbacks.on_gathering_done(sdp);
    });
}

void IceTransport::on_juice_recv([[maybe_unused]] juice_agent_t* agent,
                                 const char* data, size_t size, void* user_ptr) {
    auto* self = static_cast<IceTransport*>(user_ptr);
    if (!self || !data || size == 0) return;

    // 必须立即拷贝：data 指针在回调返回后即失效
    std::string buffer(data, size);

    auto weak = self->weak_from_this();
    boost::asio::post(self->m_io_context, [weak, buffer = std::move(buffer)]() {
        auto sp = weak.lock();
        if (!sp || !sp->m_callbacks.on_data_received) return;
        sp->m_callbacks.on_data_received(buffer.data(), buffer.size());
    });
}

// --- io_context 线程内的实际处理 ---
void IceTransport::handle_state_changed_on_loop(juice_state_t state) {
    IceState new_state = IceState::New;
    bool need_update_type = false;

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
            need_update_type = true;
            break;
        case JUICE_STATE_COMPLETED:
            new_state = IceState::Completed;
            need_update_type = true;
            break;
        case JUICE_STATE_FAILED:
            new_state = IceState::Failed;
            break;
        default:
            // 未识别的 libjuice 状态：上游升级后可能新增枚举值。
            // 不向上抛伪 New 状态，避免上层误判 ICE 重新开始。
            if (g_logger) {
                g_logger->warn("[IceTransport] 收到未知 juice_state={}，已忽略（请检查 libjuice 是否升级）",
                               static_cast<int>(state));
            }
            return;
    }

    // 必须先 store 新状态，再调 update_connection_type()——
    // 因为后者内部的 get_selected_candidates() 现在会前置检查 is_connected()，
    // 依赖 m_state 已经反映 Connected/Completed。
    m_state.store(new_state);

    if (need_update_type) {
        update_connection_type();
    }

    if (m_callbacks.on_state_changed) {
        m_callbacks.on_state_changed(new_state);
    }
}

void IceTransport::update_connection_type() {
    // 在 io_context 线程内调用，可安全访问 libjuice 只读 API
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
