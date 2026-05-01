#include "VeritasSync/net/KcpSession.h"
#include "VeritasSync/common/Logger.h"

#include <cstring>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// KcpSession 实现
// ═══════════════════════════════════════════════════════════════

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

KcpSession::KcpSession(uint32_t conv, KcpSessionCallbacks callbacks)
    : m_conv(conv), m_callbacks(std::move(callbacks)) {
}

KcpSession::~KcpSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // m_kcp 先释放（ikcp_release），此后不再有回调触发
    // m_ctx 后释放（成员逆序析构），确保回调执行期间上下文有效
    m_kcp.reset();
}

bool KcpSession::initialize(const KcpConfig& config) {
    ikcpcb* raw_kcp = ikcp_create(m_conv, nullptr);
    if (!raw_kcp) {
        if (g_logger) g_logger->error("[KcpSession] ikcp_create failed");
        return false;
    }

    // 创建回调上下文，生命周期绑定到 KcpSession 成员
    m_ctx = std::make_unique<KcpContext>();
    m_ctx->session = shared_from_this();

    // 设置回调：user 指向 m_ctx，回调中通过 weak_ptr 安全访问
    raw_kcp->user = m_ctx.get();
    raw_kcp->output = &KcpSession::kcp_output_callback;

    // 配置 KCP
    ikcp_nodelay(raw_kcp, config.nodelay, config.interval, config.resend, config.nc);
    ikcp_wndsize(raw_kcp, config.snd_wnd, config.rcv_wnd);
    ikcp_setmtu(raw_kcp, config.mtu);
    raw_kcp->rx_minrto = config.min_rto;

    m_kcp.reset(raw_kcp);
    return true;
}

int KcpSession::input(const char* data, size_t size) {
    if (size > static_cast<size_t>(LONG_MAX)) {
        if (g_logger) {
            g_logger->error("[KcpSession] input() size {} exceeds LONG_MAX", size);
        }
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_input(m_kcp.get(), data, static_cast<long>(size));
}

int KcpSession::send(const char* data, size_t size) {
    if (size > static_cast<size_t>(INT_MAX)) {
        if (g_logger) {
            g_logger->error("[KcpSession] send() size {} exceeds INT_MAX", size);
        }
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_send(m_kcp.get(), data, static_cast<int>(size));
}

int KcpSession::send(const std::string& message) {
    return send(message.data(), message.size());
}

void KcpSession::update(uint32_t current_ms) {
    std::vector<std::string> messages;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_kcp) return;

        ikcp_update(m_kcp.get(), current_ms);

        // 在同一把锁内收取已就绪的消息
        std::vector<char> buffer;
        while (true) {
            int peek_size = ikcp_peeksize(m_kcp.get());
            if (peek_size < 0) break;

            buffer.resize(peek_size);
            int recv_len = ikcp_recv(m_kcp.get(), buffer.data(), peek_size);
            if (recv_len > 0) {
                messages.emplace_back(buffer.data(), recv_len);
            } else {
                break;
            }
        }
    }

    // 锁外触发回调，允许回调内安全调用 send() 等方法
    if (m_callbacks.on_message_received) {
        for (const auto& msg : messages) {
            m_callbacks.on_message_received(msg);
        }
    }
}

uint32_t KcpSession::check(uint32_t current_ms) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return current_ms;
    return ikcp_check(m_kcp.get(), current_ms);
}

std::vector<std::string> KcpSession::receive() {
    std::vector<std::string> messages;
    std::vector<char> buffer;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return messages;

    while (true) {
        int peek_size = ikcp_peeksize(m_kcp.get());
        if (peek_size < 0) break;

        buffer.resize(peek_size);
        int recv_len = ikcp_recv(m_kcp.get(), buffer.data(), peek_size);
        if (recv_len > 0) {
            messages.emplace_back(buffer.data(), recv_len);
        } else {
            break;
        }
    }

    return messages;
}

void KcpSession::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_flush(m_kcp.get());
    }
}

int KcpSession::get_wait_send_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return 0;
    return ikcp_waitsnd(m_kcp.get());
}

int KcpSession::kcp_output_callback(const char* buf, int len,
                                    [[maybe_unused]] ikcpcb* kcp, void* user) {
    auto* ctx = static_cast<KcpContext*>(user);
    if (!ctx) return -1;

    auto session = ctx->session.lock();
    if (!session) return -1;

    if (session->m_callbacks.on_output) {
        return session->m_callbacks.on_output(buf, len);
    }
    return 0;
}

} // namespace VeritasSync
