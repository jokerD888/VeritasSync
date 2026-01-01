#include "VeritasSync/net/KcpSession.h"
#include "VeritasSync/common/Logger.h"

#include <cstring>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 工厂方法 - 创建 KcpSession 实例
// ═══════════════════════════════════════════════════════════════

/**
 * 功能：创建并初始化 KCP 会话
 * 参数：
 *   - conv: 会话 ID（两端必须一致，类似 TCP 端口）
 *   - callbacks: 回调函数集合（on_output, on_message_received）
 *   - config: KCP 配置参数（窗口大小、重传策略等）
 * 返回：成功返回 shared_ptr，失败返回 nullptr
 * 实现：使用派生类技巧突破 private 构造函数限制
 */
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

// ═══════════════════════════════════════════════════════════════
// 构造与析构
// ═══════════════════════════════════════════════════════════════

/**
 * 功能：私有构造函数，初始化成员变量
 * 参数：
 *   - conv: 会话 ID
 *   - callbacks: 回调函数（移动语义避免拷贝）
 * 说明：m_kcp 在 initialize() 中创建
 */
KcpSession::KcpSession(uint32_t conv, KcpSessionCallbacks callbacks)
    : m_conv(conv), m_callbacks(std::move(callbacks)) {
}

/**
 * 功能：析构函数，释放 KCP 资源
 * 实现：
 *   - 加锁保证线程安全
 *   - 调用 ikcp_release() 释放底层 C 结构体
 *   - RAII 自动管理资源生命周期
 */
KcpSession::~KcpSession() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_release(m_kcp);
        m_kcp = nullptr;
    }
}

/**
 * 功能：初始化 KCP 对象并配置参数
 * 参数：config - KCP 配置（极速模式、窗口、MTU等）
 * 返回：成功返回 true，失败返回 false
 * 配置项：
 *   1. 设置输出回调（KCP 需要发送数据时触发）
 *   2. 极速模式（nodelay、interval、resend、nc）
 *   3. 收发窗口大小
 *   4. MTU（适配不同网络环境）
 *   5. 最小 RTO（重传超时时间）
 */
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
    
    // 设置 MTU（适配不同网络环境）
    ikcp_setmtu(m_kcp, config.mtu);
    
    // 降低最小 RTO
    m_kcp->rx_minrto = config.min_rto;
    
    return true;
}

// ═══════════════════════════════════════════════════════════════
// 数据输入接口
// ═══════════════════════════════════════════════════════════════

/**
 * 功能：将底层收到的 UDP 数据喂给 KCP
 * 参数：
 *   - data: 数据指针
 *   - size: 数据大小
 * 返回：0 成功，其他失败
 * 调用时机：ICE 层收到数据时调用
 * 实现：加锁调用 ikcp_input()，KCP 内部处理 ACK、重组分片
 */
int KcpSession::input(const char* data, size_t size) {
    // 防止溢出：ikcp_input 使用 long 类型
    if (size > static_cast<size_t>(LONG_MAX)) {
        if (g_logger) {
            g_logger->error("[KcpSession] input() size {} exceeds LONG_MAX", size);
        }
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_input(m_kcp, data, static_cast<long>(size));
}

// ═══════════════════════════════════════════════════════════════
// 数据发送接口
// ═══════════════════════════════════════════════════════════════

/**
 * 功能：发送应用层数据（可靠传输）
 * 参数：
 *   - data: 数据指针
 *   - size: 数据大小
 * 返回：0 成功，其他失败
 * 实现：
 *   - 加锁调用 ikcp_send()，将数据加入发送队列
 *   - 需要稍后调用 update() 驱动实际发送
 *   - KCP 自动切片、重传，确保可靠到达
 */
int KcpSession::send(const char* data, size_t size) {
    // 防止溢出：ikcp_send 使用 int 类型
    if (size > static_cast<size_t>(INT_MAX)) {
        if (g_logger) {
            g_logger->error("[KcpSession] send() size {} exceeds INT_MAX", size);
        }
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return -1;
    return ikcp_send(m_kcp, data, static_cast<int>(size));
}

/*
发送链路：

应用层调用 session->send("Hello World", 11)
  ↓
KcpSession::send() - 加锁
  ↓
ikcp_send() - 加入发送队列
  ↓
(需要稍后调用 update() 驱动发送)
  ↓
ikcp_update() - 处理发送队列
  ↓
kcp_output_callback() - 静态回调
  ↓
on_output() - 用户回调
  ↓
IceTransport::send() - 实际发送 UDP 包
*/

/**
 * 功能：发送字符串消息（便捷接口）
 * 参数：message - 字符串消息
 * 返回：转发到 send(const char*, size_t)
 */
int KcpSession::send(const std::string& message) {
    return send(message.data(), message.size());
}

// ═══════════════════════════════════════════════════════════════
// KCP 状态驱动
// ═══════════════════════════════════════════════════════════════

/**
 * 功能：驱动 KCP 状态机（处理发送、重传、ACK）
 * 参数：current_ms - 当前时间戳（毫秒）
 * 调用频率：建议 5-20ms 定时调用
 * 实现：
 *   - 触发发送队列中的数据
 *   - 处理超时重传
 *   - 更新拥塞窗口
 * 优化：配合 check() 使用可降低 CPU 占用
 */
void KcpSession::update(uint32_t current_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        ikcp_update(m_kcp, current_ms);
    }
}

/**
 * 功能：获取下次需要调用 update() 的时间戳
 * 参数：current_ms - 当前时间戳（毫秒）
 * 返回：下次更新的时间戳
 * 用途：动态调整定时器，减少无效的 CPU 占用
 * 示例：
 *   uint32_t next = session->check(now);
 *   uint32_t delay = next - now;
 *   timer.expires_after(std::chrono::milliseconds(delay));
 */
uint32_t KcpSession::check(uint32_t current_ms) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return current_ms;
    return ikcp_check(m_kcp, current_ms);
}

/*
接收链路：

网络收到 UDP 包
  ↓
IceTransport::on_data_received
  ↓
session->input(data, size)  // 喂给 KCP
  ↓
ikcp_input() - 处理 ACK、重组分片
  ↓
(数据在 KCP 内部缓冲)
  ↓
session->receive()  // 主动拉取
  ↓
ikcp_peeksize() - 检查是否有完整包
  ↓
ikcp_recv() - 取出数据
  ↓
on_message_received 回调
*/

/**
 * 功能：接收完整的应用层消息
 * 返回：消息列表（每个元素是一个完整消息）
 * 调用时机：在 update() 之后调用
 * 实现：
 *   1. 持锁循环调用 ikcp_peeksize() 检查是否有完整包
 *   2. 使用 ikcp_recv() 取出数据
 *   3. 收集所有消息到容器
 *   4. 释放锁后批量触发 on_message_received 回调（防止死锁）
 * 关键修复：回调在锁外执行，用户可在回调中调用 send()
 */
std::vector<std::string> KcpSession::receive() {
    std::vector<std::string> messages;
    
    // ⚠️ 关键修复：先在锁内收集消息，释放锁后再触发回调
    // 这样避免用户在回调中调用 send() 等方法导致的死锁
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_kcp) return messages;
        
        while (true) {
            // 偷看下一个包的大小
            int peek_size = ikcp_peeksize(m_kcp);
            if (peek_size < 0) break;       // 没有完整包了
            
            // 动态分配缓冲区
            std::vector<char> buffer(peek_size);
            
            // 真正接收
            int recv_len = ikcp_recv(m_kcp, buffer.data(), peek_size);
            if (recv_len > 0) {
                messages.emplace_back(buffer.data(), recv_len);
            } else {
                break;
            }
        }
    } // 锁在这里释放
    
    // 锁释放后，批量触发回调，防止死锁
    if (m_callbacks.on_message_received) {
        for (const auto& msg : messages) {
            m_callbacks.on_message_received(msg);
        }
    }
    
    return messages;
}

/**
 * 功能：获取待发送队列的包数量
 * 返回：等待发送的包数量
 * 用途：
 *   - 流控判断（队列过长时暂停发送）
 *   - 性能监控（判断网络拥塞程度）
 * 示例：
 *   if (session->get_wait_send_count() > 1000) {
 *       // 队列拥塞，暂停发送
 *   }
 */
int KcpSession::get_wait_send_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp) return 0;
    return ikcp_waitsnd(m_kcp);
}

// ═══════════════════════════════════════════════════════════════
// KCP 输出回调 - C 到 C++ 的桥接
// ═══════════════════════════════════════════════════════════════

/*
ikcp 需要发送数据
  ↓
调用 kcp->output() - C 风格函数指针
  ↓
kcp_output_callback() - 静态函数
  ↓
通过 user_ptr 找到 KcpSession 实例
  ↓
调用 on_output - C++ std::function
  ↓
用户代码
*/

/**
 * 功能：KCP 输出回调（静态函数）
 * 参数：
 *   - buf: 要发送的数据
 *   - len: 数据长度
 *   - kcp: KCP 对象指针（未使用）
 *   - user: 用户指针（指向 KcpSession 实例）
 * 返回：0 成功
 * 实现：
 *   1. 通过 user_ptr 找到 KcpSession 实例
 *   2. 调用用户设置的 on_output 回调
 *   3. 由上层负责将数据发送到 ICE 传输层
 * 对应：KcpSessionCallbacks::on_output
 */
int KcpSession::kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user) {
    auto* self = static_cast<KcpSession*>(user);
    if (self && self->m_callbacks.on_output) {
        return self->m_callbacks.on_output(buf, len);
    }
    return 0;
}

} // namespace VeritasSync
