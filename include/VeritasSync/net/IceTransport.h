#pragma once

#include <juice/juice.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace VeritasSync {

/**
 * @brief ICE 连接状态
 */
enum class IceState {
    New,          // 初始状态
    Gathering,    // 正在收集本地候选地址
    Connecting,   // 正在尝试连接
    Connected,    // 已连接
    Completed,    // 连接优化完成
    Failed,       // 连接失败
    Disconnected  // 连接断开
};

/**
 * @brief ICE 连接类型
 */
enum class IceConnectionType {
    None,   // 未连接
    Direct, // 直连 (host 或 srflx)
    Relay   // 中继 (relay)
};

/**
 * @brief ICE 传输配置
 */
struct IceConfig {
    std::string stun_host = "stun.l.google.com";
    uint16_t stun_port = 19302;
    
    std::string turn_host;
    uint16_t turn_port = 3478;
    std::string turn_username;
    std::string turn_password;
};

/**
 * @brief IceTransport 回调接口
 * 
 * 这些回调将在 libjuice 内部线程触发，
 * 调用方应使用 boost::asio::post 投递到自己的 IO 线程。
 */
struct IceTransportCallbacks {
    // ICE 状态变化回调
    std::function<void(IceState state)> on_state_changed;
    
    // 本地候选地址准备好 (需发送给 Signaling Server)
    std::function<void(const std::string& candidate_sdp)> on_local_candidate;
    
    // 候选收集完成 (可发送完整 SDP Offer/Answer)
    std::function<void(const std::string& local_description)> on_gathering_done;
    
    // 收到数据 (ICE 连接上的原始字节数据)
    std::function<void(const char* data, size_t size)> on_data_received;
};

/**
 * @brief IceTransport - 封装 libjuice 的 ICE 传输层
 * 
 * 职责：
 * 1. 管理 juice_agent_t 的生命周期
 * 2. 处理 ICE 候选收集和交换
 * 3. 提供数据发送接口
 * 4. 隔离 libjuice 的 C 回调风格
 * 
 * 线程安全性：
 * - 所有 libjuice 回调在其内部线程执行
 * - 上层应通过回调中的 post 机制切换到主线程
 */
class IceTransport : public std::enable_shared_from_this<IceTransport> {
public:
    /**
     * @brief 创建 IceTransport 实例
     * @param config ICE 配置 (STUN/TURN)
     * @param callbacks 事件回调
     * @return 智能指针，创建失败返回 nullptr
     */
    static std::shared_ptr<IceTransport> create(
        const IceConfig& config,
        IceTransportCallbacks callbacks);
    
    ~IceTransport();
    
    // 禁止拷贝
    IceTransport(const IceTransport&) = delete;
    IceTransport& operator=(const IceTransport&) = delete;
    
    /**
     * @brief 开始收集本地候选地址
     * 成功后会触发 on_local_candidate 和 on_gathering_done 回调
     */
    void gather_candidates();
    
    /**
     * @brief 设置远端 SDP 描述
     * @param remote_sdp 对方的 SDP Offer/Answer
     */
    void set_remote_description(const std::string& remote_sdp);
    
    /**
     * @brief 添加远端候选地址
     * @param candidate 对方的 ICE candidate
     */
    void add_remote_candidate(const std::string& candidate);
    
    /**
     * @brief 通知远端候选收集完成
     */
    void set_remote_gathering_done();
    
    /**
     * @brief 获取本地 SDP 描述
     * @return 本地 SDP 字符串，失败返回空
     */
    std::string get_local_description() const;
    
    /**
     * @brief 获取当前选定的候选对
     * @param out_local 输出本地候选
     * @param out_remote 输出远端候选
     * @return 成功返回 true
     */
    bool get_selected_candidates(std::string& out_local, std::string& out_remote) const;
    
    /**
     * @brief 发送数据
     * @param data 数据指针
     * @param size 数据大小
     * @return 发送成功返回 0
     */
    int send(const char* data, size_t size);
    
    /**
     * @brief 发送数据 (span 版本)
     */
    int send(std::span<const char> data) {
        return send(data.data(), data.size());
    }
    
    /**
     * @brief 获取当前状态
     */
    IceState get_state() const { return m_state.load(); }
    
    /**
     * @brief 获取连接类型
     */
    IceConnectionType get_connection_type() const { return m_connection_type.load(); }
    
    /**
     * @brief 检查是否已连接
     */
    bool is_connected() const;
    
    /**
     * @brief 替换回调（用于两阶段初始化）
     * 
     * 在 PeerController 的 bind_callbacks() 中调用，
     * 此时 shared_ptr 已就绪，可以用 weak_ptr 绑定回调。
     * 
     * @param callbacks 新的回调接口
     */
    void set_callbacks(IceTransportCallbacks callbacks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks = std::move(callbacks);
    }
    
    /**
     * @brief 获取底层 agent 指针 (仅供兼容旧代码，后续应移除)
     */
    juice_agent_t* get_raw_agent() const { return m_agent; }

private:
    IceTransport(IceTransportCallbacks callbacks);
    
    bool initialize(const IceConfig& config);
    
    // libjuice 静态回调
    static void on_juice_state_changed(juice_agent_t* agent, juice_state_t state, void* user_ptr);
    static void on_juice_candidate(juice_agent_t* agent, const char* sdp, void* user_ptr);
    static void on_juice_gathering_done(juice_agent_t* agent, void* user_ptr);
    static void on_juice_recv(juice_agent_t* agent, const char* data, size_t size, void* user_ptr);
    
    // 内部处理
    void handle_state_changed(juice_state_t state);
    void update_connection_type();
    
    juice_agent_t* m_agent = nullptr;
    IceTransportCallbacks m_callbacks;
    
    // 【安全】保存配置字符串副本，确保 c_str() 指针在 agent 生命周期内有效
    std::string m_stun_host;
    std::string m_turn_host;
    std::string m_turn_username;
    std::string m_turn_password;
    
    std::atomic<IceState> m_state{IceState::New};
    std::atomic<IceConnectionType> m_connection_type{IceConnectionType::None};
    
    mutable std::mutex m_mutex;
};

} // namespace VeritasSync
