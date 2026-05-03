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
    // m_self_weak 后析构（成员逆序析构），确保回调执行期间 weak_ptr 始终有效
    m_kcp.reset();
}

bool KcpSession::initialize(const KcpConfig& config) {
    ikcpcb* raw_kcp = ikcp_create(m_conv, nullptr);
    if (!raw_kcp) {
        if (g_logger) g_logger->error("[KcpSession] ikcp_create failed");
        return false;
    }

    // 保存 weak_ptr，供 KCP output 回调安全访问
    // 生命周期安全：m_kcp 析构（ikcp_release）在 m_self_weak 之前，回调不会在之后触发
    m_self_weak = shared_from_this();
    raw_kcp->user = &m_self_weak;
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
    std::function<void()> drain_cb;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_kcp) return;

        ikcp_update(m_kcp.get(), current_ms);

        // drain 检测：队列从高于阈值降到阈值以下时触发一次
        if (m_on_drain && !m_drain_notified) {
            if (ikcp_waitsnd(m_kcp.get()) <= m_drain_threshold) {
                m_drain_notified = true;
                drain_cb = m_on_drain;
            }
        }

        // 快速检查：无完整消息则跳过收取，避免 messages 分配
        if (ikcp_peeksize(m_kcp.get()) < 0) {
            // 即使无消息，也需要在锁外触发 drain 回调
            if (drain_cb) drain_cb();
            return;
        }

        // thread_local buffer 复用，避免高频分配（每 10ms 调一次）
        thread_local std::vector<char> t_buffer;
        while (true) {
            int peek_size = ikcp_peeksize(m_kcp.get());
            if (peek_size < 0) break;

            t_buffer.resize(peek_size);
            int recv_len = ikcp_recv(m_kcp.get(), t_buffer.data(), peek_size);
            if (recv_len > 0) {
                messages.emplace_back(t_buffer.data(), recv_len);
            } else {
                break;
            }
        }
    }

    // 锁外触发回调，允许回调内安全调用 send() 等方法
    if (drain_cb) drain_cb();
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

void KcpSession::set_on_drain(std::function<void()> on_drain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending_drain = nullptr;  // 清除未取出的旧 pending
    m_on_drain = std::move(on_drain);
    m_drain_notified = false;
    // 如果队列已在阈值以下，标记为待触发（不直接调用，避免在调用者锁内执行回调）
    if (m_kcp && ikcp_waitsnd(m_kcp.get()) <= m_drain_threshold) {
        m_drain_notified = true;
        m_pending_drain = m_on_drain;
    }
}

void KcpSession::clear_on_drain() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_drain = nullptr;
    m_pending_drain = nullptr;
    m_drain_notified = false;
}

void KcpSession::set_drain_threshold(int threshold) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_drain_threshold = threshold;
}

int KcpSession::get_drain_threshold() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_drain_threshold;
}

std::function<void()> KcpSession::take_pending_drain() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(m_pending_drain);
}

int KcpSession::kcp_output_callback(const char* buf, int len,
                                    [[maybe_unused]] ikcpcb* kcp, void* user) {
    auto* self_weak = static_cast<std::weak_ptr<KcpSession>*>(user);
    if (!self_weak) return -1;

    auto session = self_weak->lock();
    if (!session) return -1;

    if (session->m_callbacks.on_output) {
        return session->m_callbacks.on_output(buf, len);
    }
    return 0;
}

} // namespace VeritasSync
