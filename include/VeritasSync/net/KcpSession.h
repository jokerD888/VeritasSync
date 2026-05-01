#pragma once

#include <ikcp.h>

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace VeritasSync {

// 前向声明
class KcpSession;

/**
 * @brief KCP 回调上下文结构体
 * 用于在 KCP 回调中安全地访问 KcpSession
 * 使用 weak_ptr 避免循环引用，同时允许检测对象是否已销毁
 *
 * 生命周期由 KcpSession 的 m_ctx 成员管理，无需全局注册表。
 */
struct KcpContext {
    std::weak_ptr<KcpSession> session;
};

/**
 * @brief KCP 会话配置
 */
struct KcpConfig {
    // nodelay 模式参数
    // 推荐配置（KCP 官方）: nodelay=1, interval=20, resend=2, nc=1
    int nodelay = 1;      // 0: 正常模式, 1: 极速模式
    int interval = 20;    // 内部时钟间隔 (ms)
    int resend = 2;       // 快速重传次数
    int nc = 1;           // 0: 启用流控, 1: 关闭流控

    // 窗口大小（单位：包数，非字节数）
    // KCP 默认 MSS = 1400 字节/包
    // 实际缓冲区大小 = wnd × MSS
    // 例如：4096 × 1400 ≈ 5.47 MB
    int snd_wnd = 4096;   // 发送窗口：同时发送的最大包数
    int rcv_wnd = 4096;   // 接收窗口：同时接收的最大包数

    // 最小 RTO
    int min_rto = 30;

    // MTU 配置（适配不同网络环境）
    // 标准以太网: 1500, PPPoE: 1492, 移动网络: 1280-1400
    int mtu = 1400;
};

/**
 * @brief KcpSession 回调接口
 */
struct KcpSessionCallbacks {
    /**
     * KCP 需要发送原始 UDP 数据的回调
     * 由上层负责通过 ICE 通道发送
     *
     * ⚠️ 警告：此回调在 KCP 内部持锁状态下触发
     * 请勿在回调中调用 KcpSession 的任何方法，否则会死锁
     * 正确做法：直接转发到底层传输层（如 IceTransport::send）
     *
     * 示例（正确）：
     *   on_output = [&ice](const char* data, int len) {
     *       return ice->send(data, len);  // ✅ 安全
     *   };
     *
     * 示例（错误）：
     *   on_output = [&session](const char* data, int len) {
     *       session->get_wait_send_count();  // ❌ 死锁！
     *       return 0;
     *   };
     */
    std::function<int(const char* data, int len)> on_output;

    /**
     * 收到完整的应用层消息的回调
     * 在 update() 内部自动触发，无需手动调用 receive()
     *
     * ✅ 安全：此回调在锁外执行，可以安全调用 send() 等方法
     *
     * 示例：
     *   on_message_received = [&session](const std::string& msg) {
     *       if (msg == "PING") {
     *           session->send("PONG");  // ✅ 安全，不会死锁
     *       }
     *   };
     */
    std::function<void(const std::string& message)> on_message_received;
};

/**
 * @brief KcpSession - 封装 ikcp 的可靠传输会话
 *
 * 职责：
 * 1. 管理 ikcpcb 的生命周期
 * 2. 处理 KCP 的更新驱动
 * 3. 提供可靠的消息发送/接收接口
 * 4. 隔离 ikcp 的 C 回调风格
 *
 * 使用模式：
 * - 上层通过 input() 喂入 ICE 收到的数据
 * - 上层通过 send() 发送应用层消息
 * - 上层定时调用 update() 驱动 KCP 状态机并自动触发 on_message_received
 * - KCP 需要发送数据时通过 on_output 回调
 */
class KcpSession : public std::enable_shared_from_this<KcpSession> {
public:
    /**
     * @brief 创建 KcpSession 实例
     * @param conv 会话 ID (两端必须一致)
     * @param callbacks 事件回调
     * @param config KCP 配置
     * @return 智能指针
     */
    static std::shared_ptr<KcpSession> create(
        uint32_t conv,
        KcpSessionCallbacks callbacks,
        const KcpConfig& config = KcpConfig{});

    ~KcpSession();

    // 禁止拷贝
    KcpSession(const KcpSession&) = delete;
    KcpSession& operator=(const KcpSession&) = delete;

    int input(const char* data, size_t size);
    int input(std::span<const char> data) {
        return input(data.data(), data.size());
    }

    int send(const char* data, size_t size);
    int send(std::span<const char> data) {
        return send(data.data(), data.size());
    }
    int send(const std::string& message);

    /**
     * @brief 驱动 KCP 状态机并投递已收到的消息
     *
     * 内部流程：ikcp_update → flush 输出 → 读取完整消息 → 触发 on_message_received（锁外）
     * 上层无需手动调用 receive()，回调会自动触发。
     *
     * @param current_ms 当前时间戳 (毫秒)
     */
    void update(uint32_t current_ms);

    uint32_t check(uint32_t current_ms) const;

    /**
     * @brief 接收应用层消息（纯 pull 模式）
     *
     * 仅返回已就绪的消息，不触发 on_message_received 回调。
     * 如果设置了 on_message_received 回调且定期调用 update()，通常无需手动调用此方法。
     *
     * @return 接收到的消息列表
     */
    std::vector<std::string> receive();

    int get_wait_send_count() const;
    uint32_t get_conv() const { return m_conv; }
    bool is_valid() const { return m_kcp != nullptr; }

    /**
     * @brief 强制刷新 KCP 发送缓冲区
     *
     * 在需要确保数据立即发出时调用（如发送 goodbye 后）
     */
    void flush();

private:
    KcpSession(uint32_t conv, KcpSessionCallbacks callbacks);
    bool initialize(const KcpConfig& config);

    static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user);

    // 自定义删除器：仅释放 KCP 资源
    // 上下文 (m_ctx) 的生命周期由 KcpSession 成员管理，析构顺序保证安全：
    // m_kcp 先析构（ikcp_release），m_ctx 后析构
    struct KcpDeleter {
        void operator()(ikcpcb* kcp) const {
            if (kcp) ikcp_release(kcp);
        }
    };

    uint32_t m_conv;
    std::unique_ptr<ikcpcb, KcpDeleter> m_kcp;
    KcpSessionCallbacks m_callbacks;
    std::unique_ptr<KcpContext> m_ctx;  // 回调上下文，生命周期与 KcpSession 绑定

    mutable std::mutex m_mutex;
};

} // namespace VeritasSync
