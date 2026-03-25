#include "VeritasSync/net/KcpSession.h"
#include "VeritasSync/common/Logger.h"

#include <cstring>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// KcpContextManager 实现
// ═══════════════════════════════════════════════════════════════

KcpContextManager& KcpContextManager::instance() {
    static KcpContextManager instance;
    return instance;
}

KcpContext* KcpContextManager::register_context(ikcpcb* kcp, std::shared_ptr<KcpSession> session) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto ctx = std::make_shared<KcpContext>();
    ctx->session = session;
    ctx->kcp = kcp;
    
    KcpContext* raw_ptr = ctx.get();
    m_contexts[kcp] = std::move(ctx);
    
    return raw_ptr;
}

void KcpContextManager::unregister_context(ikcpcb* kcp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_contexts.erase(kcp);
}

std::shared_ptr<KcpContext> KcpContextManager::get_context(ikcpcb* kcp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_contexts.find(kcp);
    if (it != m_contexts.end()) {
        return it->second;
    }
    return nullptr;
}

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
    m_destroyed.store(true);
    // 【修复】m_kcp 使用自定义删除器，会自动注销上下文并释放 KCP
    m_kcp.reset();
}

bool KcpSession::initialize(const KcpConfig& config) {
    // 先创建 KCP 对象（使用裸指针，稍后交给 unique_ptr）
    ikcpcb* raw_kcp = ikcp_create(m_conv, nullptr);
    if (!raw_kcp) {
        if (g_logger) g_logger->error("[KcpSession] ikcp_create failed");
        return false;
    }
    
    // 【修复】在设置回调前注册上下文，确保回调执行时上下文有效
    auto& manager = KcpContextManager::instance();
    KcpContext* ctx = manager.register_context(raw_kcp, shared_from_this());
    if (!ctx) {
        ikcp_release(raw_kcp);
        return false;
    }
    
    // 更新 KCP 的 user 指针
    raw_kcp->user = ctx;
    
    // 设置输出回调
    raw_kcp->output = &KcpSession::kcp_output_callback;
    
    // 配置 KCP
    ikcp_nodelay(raw_kcp, config.nodelay, config.interval, config.resend, config.nc);
    ikcp_wndsize(raw_kcp, config.snd_wnd, config.rcv_wnd);
    ikcp_setmtu(raw_kcp, config.mtu);
    raw_kcp->rx_minrto = config.min_rto;
    
    // 将 KCP 交给 unique_ptr 管理（使用自定义删除器）
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_update(m_kcp.get(), current_ms);
    }
}

uint32_t KcpSession::check(uint32_t current_ms) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return current_ms;
    return ikcp_check(m_kcp.get(), current_ms);
}

std::vector<std::string> KcpSession::receive() {
    std::vector<std::string> messages;
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_kcp) return messages;
        
        while (true) {
            int peek_size = ikcp_peeksize(m_kcp.get());
            if (peek_size < 0) break;
            
            std::vector<char> buffer(peek_size);
            
            int recv_len = ikcp_recv(m_kcp.get(), buffer.data(), peek_size);
            if (recv_len > 0) {
                messages.emplace_back(buffer.data(), recv_len);
            } else {
                break;
            }
        }
    }
    
    if (m_callbacks.on_message_received) {
        for (const auto& msg : messages) {
            m_callbacks.on_message_received(msg);
        }
    }
    
    return messages;
}

int KcpSession::get_wait_send_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return 0;
    return ikcp_waitsnd(m_kcp.get());
}

// 【修复】改进的回调函数，通过 KcpContext 的 weak_ptr 保证生命周期安全
int KcpSession::kcp_output_callback(const char* buf, int len,
                                    [[maybe_unused]] ikcpcb* kcp, void* user) {
    auto* ctx = static_cast<KcpContext*>(user);
    if (!ctx) return -1;

    // 直接使用 ctx->session.lock() 提升为 shared_ptr，零额外锁开销。
    // weak_ptr::lock() 本身已是线程安全的原子操作，无需再经过全局管理器查找。
    auto session = ctx->session.lock();
    if (!session) {
        return -1;  // 对象已销毁，安全返回
    }

    if (session->m_callbacks.on_output) {
        return session->m_callbacks.on_output(buf, len);
    }
    return 0;
}

} // namespace VeritasSync
