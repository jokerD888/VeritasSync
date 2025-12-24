#pragma once

#include <ikcp.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace VeritasSync {

/**
 * @brief KCP 会话配置
 */
struct KcpConfig {
    // nodelay 模式参数
    int nodelay = 1;      // 0: 正常模式, 1: 极速模式
    int interval = 5;     // 内部时钟间隔 (ms)
    int resend = 2;       // 快速重传次数
    int nc = 1;           // 0: 启用流控, 1: 关闭流控
    
    // 窗口大小
    int snd_wnd = 4096;
    int rcv_wnd = 4096;
    
    // 最小 RTO
    int min_rto = 10;
};

/**
 * @brief KcpSession 回调接口
 */
struct KcpSessionCallbacks {
    // KCP 需要发送原始 UDP 数据的回调
    // 由上层负责通过 ICE 通道发送
    std::function<int(const char* data, int len)> on_output;
    
    // 收到完整的应用层消息的回调
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
    
    /**
     * @brief 获取底层 KCP 指针 (仅供兼容旧代码，后续应移除)
     */
    ikcpcb* get_raw_kcp() const { return m_kcp; }

private:
    KcpSession(uint32_t conv, KcpSessionCallbacks callbacks);
    
    bool initialize(const KcpConfig& config);
    
    // KCP 输出回调
    static int kcp_output_callback(const char* buf, int len, ikcpcb* kcp, void* user);
    
    uint32_t m_conv;
    ikcpcb* m_kcp = nullptr;
    KcpSessionCallbacks m_callbacks;
    
    mutable std::mutex m_mutex;
};

} // namespace VeritasSync
