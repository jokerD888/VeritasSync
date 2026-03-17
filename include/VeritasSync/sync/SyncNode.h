#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "VeritasSync/common/Config.h"

namespace VeritasSync {

// 前向声明，减少头文件依赖
class P2PManager;
class TrackerClient;
class StateManager;

/**
 * @brief SyncNode - 同步任务的核心协调器
 * 
 * ⚠️ 重要：SyncNode 使用 enable_shared_from_this，必须通过 shared_ptr 创建！
 * 
 * 【正确用法】
 * @code
 * auto node = SyncNode::create(task, config);
 * node->start();
 * // ... 使用中
 * node->stop();
 * @endcode
 * 
 * 【错误用法】
 * @code
 * SyncNode node(task, config);  // ❌ 编译错误：构造函数是 private
 * auto node = std::make_unique<SyncNode>(task, config);  // ❌ 编译错误
 * @endcode
 * 
 * 【设计原因】
 * SyncNode 的 start() 方法会注册异步回调到其他模块（如 TrackerClient）。
 * 这些回调可能在 SyncNode 销毁后仍在执行。
 * 使用 weak_ptr 捕获 SyncNode 可以安全地检测对象是否还存活，防止 Use-After-Free。
 * 
 */

class SyncNode : public std::enable_shared_from_this<SyncNode> {
public:
    // 🔑 工厂方法：强制使用 shared_ptr 创建（支持 enable_shared_from_this）
    static std::shared_ptr<SyncNode> create(SyncTask task, const Config& global_config) {
        // 使用 new 而不是 make_shared，因为构造函数是 private
        return std::shared_ptr<SyncNode>(new SyncNode(std::move(task), global_config));
    }

    ~SyncNode();

    std::shared_ptr<P2PManager> get_p2p();
    std::string get_key() const;
    std::string get_root_path() const;

    // 【修复】start返回bool表示启动是否成功
    bool start();
    void stop();  // 优雅地停止同步任务

    bool is_tracker_online() const;
    bool is_started() const;  // 检查是否已启动

private:
    // 🔒 构造函数设为 private，强制通过 create() 创建
    SyncNode(SyncTask task, const Config& global_config);
    SyncTask m_task;
    Config m_global_config;

    // 使用 C++20 原子智能指针（线程安全且高效）
    std::atomic<std::shared_ptr<TrackerClient>> m_tracker_client;
    std::atomic<std::shared_ptr<P2PManager>> m_p2p_manager;
    std::unique_ptr<StateManager> m_state_manager;
    
    // 【修复问题6】使用 std::call_once 简化双重停止保护
    // 替代原来的 m_started + m_is_stopping 两个原子变量
    std::once_flag m_stop_once;           // 确保 stop() 只执行一次
    std::atomic<bool> m_started{false};   // 防止重复启动（供 is_started() 查询）
    std::atomic<bool> m_is_stopping{false};  // 标记正在停止（供回调检查）
};

}  // namespace VeritasSync