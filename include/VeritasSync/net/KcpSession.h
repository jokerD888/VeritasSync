#pragma once

#include <ikcp.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>

namespace VeritasSync {

// 前向声明
class KcpSession;

/**
 * @brief KCP 回调上下文结构体
 * 用于在 KCP 回调中安全地访问 KcpSession
 * 使用 weak_ptr 避免循环引用，同时允许检测对象是否已销毁
 */
struct KcpContext {
    std::weak_ptr<KcpSession> session;
    ikcpcb* kcp = nullptr;  // 反向引用，用于注销
};

/**
 * @brief KCP 上下文管理器（全局单例）
 * 管理所有 KCP 上下文的生命周期，确保回调执行期间上下文有效
 */
class KcpContextManager {
public:
    static KcpContextManager& instance();
    
    // 注册上下文，返回原始指针给 KCP 库使用
    KcpContext* register_context(ikcpcb* kcp, std::shared_ptr<KcpSession> session);
    
    // 注销上下文（在 KCP 释放时调用）
    void unregister_context(ikcpcb* kcp);

private:
    KcpContextManager() = default;
    ~KcpContextManager() = default;
    KcpContextManager(const KcpContextManager&) = delete;
    KcpContextManager& operator=(const KcpContextManager&) = delete;
    
    std::mutex m_mutex;
    std::unordered_map<ikcpcb*, std::shared_ptr<KcpContext>> m_contexts;
};

/**
 * @brief KCP 会话配置
 */
struct KcpConfig {
    // nodelay 模式参数
    int nodelay = 1;      // 0: 正常模式, 1: 极速模式
    int interval = 5;     // 内部时钟间隔 (ms)
    int resend = 2;       // 快速重传次数
    int nc = 1;           // 0: 启用流控, 1: 关闭流控
    
    // 窗口大小（单位：包数，非字节数）
    // KCP 默认 MSS = 1400 字节/包
    // 实际缓冲区大小 = wnd × MSS
    // 例如：4096 × 1400 ≈ 5.47 MB
    int snd_wnd = 4096;   // 发送窗口：同时发送的最大包数
    int rcv_wnd = 4096;   // 接收窗口：同时接收的最大包数
    
    // 最小 RTO
    int min_rto = 10;
    
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
 * - 上层定时调用 update() 驱动 KCP 状态机
 * - KCP 需要发送数据时通过 on_output 回调
 * - 收到完整消息时通过 on_message_received 回调
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
    
    /**
     * @brief 输入底层收到的数据
     * 
     * 当 ICE 层收到数据后，调用此方法喂给 KCP
     * 
     * @param data 数据指针
     * @param size 数据大小
     * @return 0 成功，其他失败
     */
    int input(const char* data, size_t size);
    
    /**
     * @brief 输入底层收到的数据 (span 版本)
     */
    int input(std::span<const char> data) {
        return input(data.data(), data.size());
    }
    
    /**
     * @brief 发送应用层消息
     * 
     * KCP 会自动切片、重传，确保可靠到达
     * 
     * @param data 数据指针
     * @param size 数据大小
     * @return 0 成功，其他失败
     */
    int send(const char* data, size_t size);
    
    /**
     * @brief 发送应用层消息 (span 版本)
     */
    int send(std::span<const char> data) {
        return send(data.data(), data.size());
    }
    
    /**
     * @brief 发送字符串消息
     */
    int send(const std::string& message);
    
    /**
     * @brief 驱动 KCP 状态机
     * 
     * 需要上层定时调用（建议 5-20ms）
     * 
     * @param current_ms 当前时间戳 (毫秒)
     */
    void update(uint32_t current_ms);
    
    /**
     * @brief 获取下次需要调用 update() 的时间戳
     * 
     * 用于动态调整定时器，减少无效的 CPU 占用
     * 
     * @param current_ms 当前时间戳 (毫秒)
     * @return 下次更新的时间戳
     */
    uint32_t check(uint32_t current_ms) const;
    
    /**
     * @brief 接收应用层消息
     * 
     * 在 update() 之后调用，获取完整的应用层消息
     * 
     * @return 接收到的消息列表
     */
    std::vector<std::string> receive();
    
    /**
     * @brief 获取待发送队列长度
     * 
     * 用于流控判断
     */
    int get_wait_send_count() const;
    
    /**
     * @brief 获取会话 ID
     */
    uint32_t get_conv() const { return m_conv; }
    
    /**
     * @brief 检查会话是否有效
     */
    bool is_valid() const { return m_kcp != nullptr; }
    
private:
    KcpSession(uint32_t conv, KcpSessionCallbacks callbacks);
    bool initialize(const KcpConfig& config);
    
    static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user);
    
    // 【修复】自定义删除器，延迟 KCP 资源释放
    struct KcpDeleter {
        void operator()(ikcpcb* kcp) const {
            if (kcp) {
                // 先注销上下文，再释放 KCP
                KcpContextManager::instance().unregister_context(kcp);
                ikcp_release(kcp);
            }
        }
    };
    
    uint32_t m_conv;
    std::unique_ptr<ikcpcb, KcpDeleter> m_kcp;  // 使用自定义删除器
    KcpSessionCallbacks m_callbacks;
    
    mutable std::mutex m_mutex;
    std::atomic<bool> m_destroyed{false};
};

} // namespace VeritasSync
