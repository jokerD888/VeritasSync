#include "VeritasSync/net/KcpSession.h"
#include "VeritasSync/common/Logger.h"

#include <cstring>

namespace VeritasSync {

// --- 工厂方法 ---
std::shared_ptr<KcpSession> KcpSession::create(
    uint32_t conv,
    KcpSessionCallbacks callbacks,
    const KcpConfig& config) {
    
    struct KcpSessionMaker : public KcpSession {
        KcpSessionMaker(uint32_t c, KcpSessionCallbacks cb) 
            : KcpSession(c, std::move(cb)) {}
    };
    
    auto session = std::make_shared<KcpSessionMaker>(conv, std::move(callbacks));
    if (!session->initialize(config)) {
        return nullptr;
    }
    return session;
}

// --- 构造与析构 ---
KcpSession::KcpSession(uint32_t conv, KcpSessionCallbacks callbacks)
    : m_conv(conv), m_callbacks(std::move(callbacks)) {
}

KcpSession::~KcpSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_release(m_kcp);
        m_kcp = nullptr;
    }
}

bool KcpSession::initialize(const KcpConfig& config) {
    m_kcp = ikcp_create(m_conv, this);
    if (!m_kcp) {
        if (g_logger) g_logger->error("[KcpSession] ikcp_create failed");
        return false;
    }
    
    // 设置输出回调
    m_kcp->output = &KcpSession::kcp_output_callback;
    
    // 配置极速模式
    ikcp_nodelay(m_kcp, config.nodelay, config.interval, config.resend, config.nc);
    
    // 扩大收发窗口
    ikcp_wndsize(m_kcp, config.snd_wnd, config.rcv_wnd);
    
    // 降低最小 RTO
    m_kcp->rx_minrto = config.min_rto;
    
    return true;
}

// --- 公共接口 ---
int KcpSession::input(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_input(m_kcp, data, static_cast<long>(size));
}

int KcpSession::send(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_send(m_kcp, data, static_cast<int>(size));
}

int KcpSession::send(const std::string& message) {
    return send(message.data(), message.size());
}

void KcpSession::update(uint32_t current_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_update(m_kcp, current_ms);
    }
}

std::vector<std::string> KcpSession::receive() {
    std::vector<std::string> messages;
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_kcp) return messages;
    
    while (true) {
        // 偷看下一个包的大小
        int peek_size = ikcp_peeksize(m_kcp);
        if (peek_size < 0) break;
        
        // 动态分配缓冲区
        std::vector<char> buffer(peek_size);
        
        // 真正接收
        int recv_len = ikcp_recv(m_kcp, buffer.data(), peek_size);
        if (recv_len > 0) {
            messages.emplace_back(buffer.data(), recv_len);
            
            // 触发回调
            if (m_callbacks.on_message_received) {
                m_callbacks.on_message_received(messages.back());
            }
        } else {
            break;
        }
    }
    
    return messages;
}

int KcpSession::get_wait_send_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return 0;
    return ikcp_waitsnd(m_kcp);
}

// --- KCP 输出回调 ---
int KcpSession::kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user) {
    auto* self = static_cast<KcpSession*>(user);
    if (self && self->m_callbacks.on_output) {
        return self->m_callbacks.on_output(buf, len);
    }
    return 0;
}

} // namespace VeritasSync
