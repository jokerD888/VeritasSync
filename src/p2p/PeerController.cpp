#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/common/Logger.h"

#include <boost/asio/post.hpp>
#include <functional>

namespace VeritasSync {

// ═══════════════════════════════════════════════════════════════
// 工厂方法
// ═══════════════════════════════════════════════════════════════

std::shared_ptr<PeerController> PeerController::create(
    const std::string& self_id,
    const std::string& peer_id,
    boost::asio::io_context& io_context,
    const IceConfig& ice_config,
    PeerControllerCallbacks callbacks) {
    
    // 使用内部派生类绕过 private 构造函数
    struct PeerControllerMaker : public PeerController {
        PeerControllerMaker(
            const std::string& self, 
            const std::string& peer, 
            boost::asio::io_context& ioc, 
            PeerControllerCallbacks cb)
            : PeerController(self, peer, ioc, std::move(cb)) {}
    };
    
    auto controller = std::make_shared<PeerControllerMaker>(
        self_id, peer_id, io_context, std::move(callbacks));
    
    // 第一阶段：创建 IceTransport（但不绑定回调）
    if (!controller->initialize_ice(ice_config)) {
        return nullptr;
    }
    
    // 第二阶段：现在 shared_ptr 已创建，可以安全使用 weak_from_this()
    controller->bind_callbacks();
    
    return controller;
}

// ═══════════════════════════════════════════════════════════════
// 构造与析构
// ═══════════════════════════════════════════════════════════════

PeerController::PeerController(
    const std::string& self_id,
    const std::string& peer_id,
    boost::asio::io_context& io_context,
    PeerControllerCallbacks callbacks)
    : m_self_id(self_id)
    , m_peer_id(peer_id)
    , m_is_offer_side(self_id < peer_id)  // 自动判断角色
    , m_io_context(io_context)
    , m_callbacks(std::move(callbacks)) {
}

PeerController::~PeerController() {
    // 确保资源被释放
    close();
}

// ═══════════════════════════════════════════════════════════════
// 初始化
// ═══════════════════════════════════════════════════════════════

bool PeerController::initialize_ice(const IceConfig& ice_config) {
    // 【关键设计】
    // IceTransport 的回调在 libjuice 内部线程触发，
    // 我们需要通过 post 投递到 io_context 线程。
    // 
    // 问题：此时 shared_ptr 尚未完全创建，不能使用 weak_from_this()
    // 解决：使用 this 指针 + io_context post，在回调中检查 m_is_valid
    // 
    // 这种设计的安全性依赖于：
    // 1. close() 会先设置 m_is_valid = false，再释放资源
    // 2. 所有回调在执行业务逻辑前检查 m_is_valid
    // 3. 回调通过 post 到 io_context，与其他操作串行化
    
    IceTransportCallbacks ice_callbacks;
    
    // 捕获 raw pointer + io_context reference
    // IceTransport 的生命周期由 PeerController 管理
    PeerController* self_ptr = this;
    auto& io = m_io_context;
    
    ice_callbacks.on_state_changed = [self_ptr, &io](IceState state) {
        // 【重要】post 到 io_context 线程执行
        boost::asio::post(io, [self_ptr, state]() {
            // 检查有效性
            if (self_ptr->m_is_valid.load()) {
                self_ptr->on_ice_state_changed(state);
            }
        });
    };
    
    ice_callbacks.on_local_candidate = [self_ptr, &io](const std::string& candidate) {
        boost::asio::post(io, [self_ptr, candidate]() {
            if (self_ptr->m_is_valid.load()) {
                self_ptr->on_ice_local_candidate(candidate);
            }
        });
    };
    
    ice_callbacks.on_gathering_done = [self_ptr, &io](const std::string& local_desc) {
        boost::asio::post(io, [self_ptr, local_desc]() {
            if (self_ptr->m_is_valid.load()) {
                self_ptr->on_ice_gathering_done(local_desc);
            }
        });
    };
    
    ice_callbacks.on_data_received = [self_ptr, &io](const char* data, size_t size) {
        // 数据接收需要拷贝，因为原始指针在回调返回后可能无效
        std::string data_copy(data, size);
        boost::asio::post(io, [self_ptr, data_copy]() {
            if (self_ptr->m_is_valid.load()) {
                self_ptr->on_ice_data_received(data_copy.data(), data_copy.size());
            }
        });
    };
    
    m_ice = IceTransport::create(ice_config, std::move(ice_callbacks));
    if (!m_ice) {
        if (g_logger) {
            g_logger->error("[PeerController] Failed to create IceTransport for {}", m_peer_id);
        }
        return false;
    }
    
    return true;
}

void PeerController::bind_callbacks() {
    // 现在 shared_ptr 已创建，可以做一些额外的初始化
    // 但由于 IceTransport 的回调已经在 initialize_ice 中设置，
    // 这里只需要记录日志
    
    if (g_logger) {
        g_logger->info("[PeerController] Initialized {} <-> {} (offer_side={})", 
                       m_self_id, m_peer_id, m_is_offer_side);
    }
}

// ═══════════════════════════════════════════════════════════════
// Conv ID 计算
// ═══════════════════════════════════════════════════════════════

uint32_t PeerController::calculate_conv() const {
    // 使用两个 ID 的有序组合，确保两端一致
    std::string id_pair = m_is_offer_side 
        ? (m_self_id + m_peer_id) 
        : (m_peer_id + m_self_id);
    
    // 实际上应该是 min + max 的顺序，更清晰：
    // std::string id_pair = (m_self_id < m_peer_id) 
    //     ? (m_self_id + m_peer_id) 
    //     : (m_peer_id + m_self_id);
    // 但 m_is_offer_side 就是 m_self_id < m_peer_id，所以等价
    
    return static_cast<uint32_t>(std::hash<std::string>{}(id_pair));
}

// ═══════════════════════════════════════════════════════════════
// 连接控制
// ═══════════════════════════════════════════════════════════════

void PeerController::initiate_connection() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_ice || !m_is_valid.load()) return;
    
    m_state.store(PeerState::Connecting);
    m_ice->gather_candidates();
    
    if (g_logger) {
        g_logger->info("[PeerController] {} initiating connection to {} (offer_side={})",
                       m_self_id, m_peer_id, m_is_offer_side);
    }
}

void PeerController::handle_signaling(const std::string& signal_type, const std::string& payload) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_ice || !m_is_valid.load()) return;
    
    if (signal_type == "ice_candidate") {
        m_ice->add_remote_candidate(payload);
    } else if (signal_type == "sdp_offer") {
        m_ice->set_remote_description(payload);
        // 收到 Offer 后开始收集候选（作为 Answer 方）
        m_state.store(PeerState::Connecting);
        m_ice->gather_candidates();
    } else if (signal_type == "sdp_answer") {
        m_ice->set_remote_description(payload);
    } else if (signal_type == "ice_gathering_done") {
        m_ice->set_remote_gathering_done();
    }
}

void PeerController::close() {
    // 先标记为无效，让所有异步操作安全退出
    m_is_valid.store(false);
    m_state.store(PeerState::Disconnected);
    
    // 取消同步超时定时器
    if (sync_timeout_timer) {
        sync_timeout_timer->cancel();
        sync_timeout_timer.reset();
    }
    
    // 释放资源
    std::lock_guard<std::mutex> lock(m_mutex);
    m_kcp.reset();
    m_ice.reset();
    m_kcp_initialized.store(false);
}

// ═══════════════════════════════════════════════════════════════
// 消息收发
// ═══════════════════════════════════════════════════════════════

int PeerController::send_message(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_kcp || !m_is_valid.load()) return -1;
    
    m_kcp->send(data, size);
    return m_kcp->get_wait_send_count();
}

int PeerController::send_message(const std::string& message) {
    return send_message(message.data(), message.size());
}

// ═══════════════════════════════════════════════════════════════
// KCP 驱动
// ═══════════════════════════════════════════════════════════════

void PeerController::update_kcp(uint32_t current_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_kcp || !m_is_valid.load()) return;
    
    m_kcp->update(current_ms);
    
    // receive() 会在锁外触发回调，这里调用是为了驱动消息接收
    // KcpSession::receive() 内部会先收集消息，释放锁后触发回调
    // 但我们持有 PeerController 的锁，不会死锁（不同的锁）
    m_kcp->receive();
}

// ═══════════════════════════════════════════════════════════════
// 状态查询
// ═══════════════════════════════════════════════════════════════

IceConnectionType PeerController::get_connection_type() const {
    std::lock_guard<std::mutex> lock(m_mutex);  // 加锁保护
    
    if (m_ice) {
        return m_ice->get_connection_type();
    }
    return IceConnectionType::None;
}

int PeerController::get_kcp_wait_send() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_kcp) {
        return m_kcp->get_wait_send_count();
    }
    return 0;
}

std::shared_ptr<IceTransport> PeerController::get_ice_transport() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ice;
}

std::shared_ptr<KcpSession> PeerController::get_kcp_session() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_kcp;
}

// ═══════════════════════════════════════════════════════════════
// ICE 回调处理
// ═══════════════════════════════════════════════════════════════

void PeerController::on_ice_state_changed(IceState state) {
    // 此方法应该在 io_context 线程中被调用（通过 post）
    
    if (!m_is_valid.load()) return;
    
    if (g_logger) {
        g_logger->info("[PeerController] {} ICE state changed to {}", 
                       m_peer_id, static_cast<int>(state));
    }
    
    if (state == IceState::Connected || state == IceState::Completed) {
        // ICE 连接成功，设置 KCP
        // 使用原子标志防止重复初始化
        bool expected = false;
        if (m_kcp_initialized.compare_exchange_strong(expected, true)) {
            setup_kcp_session();
            
            // 记录连接时间
            auto now = std::chrono::system_clock::now();
            connected_at_ts.store(
                std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count());
        }
        
        m_state.store(PeerState::Connected);
        if (m_callbacks.on_state_changed) {
            m_callbacks.on_state_changed(PeerState::Connected);
        }
        
    } else if (state == IceState::Failed) {
        m_state.store(PeerState::Failed);
        if (m_callbacks.on_state_changed) {
            m_callbacks.on_state_changed(PeerState::Failed);
        }
        
    } else if (state == IceState::Disconnected) {
        m_state.store(PeerState::Disconnected);
        if (m_callbacks.on_state_changed) {
            m_callbacks.on_state_changed(PeerState::Disconnected);
        }
    }
}

void PeerController::on_ice_local_candidate(const std::string& candidate) {
    if (!m_is_valid.load()) return;
    
    if (m_callbacks.on_signal_needed) {
        m_callbacks.on_signal_needed("ice_candidate", candidate);
    }
}

void PeerController::on_ice_gathering_done(const std::string& local_desc) {
    if (!m_is_valid.load()) return;
    
    if (m_callbacks.on_signal_needed) {
        // 根据角色发送正确的信令类型
        std::string signal_type = m_is_offer_side ? "sdp_offer" : "sdp_answer";
        m_callbacks.on_signal_needed(signal_type, local_desc);
        
        // 同时发送 gathering_done 信号
        m_callbacks.on_signal_needed("ice_gathering_done", "");
    }
}

void PeerController::on_ice_data_received(const char* data, size_t size) {
    if (!m_is_valid.load()) return;
    
    // ICE 收到的数据喂给 KCP
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        m_kcp->input(data, size);
    }
}

// ═══════════════════════════════════════════════════════════════
// KCP 回调处理
// ═══════════════════════════════════════════════════════════════

int PeerController::on_kcp_output(const char* data, int len) {
    // 此回调在 KCP 内部触发，不能调用 KCP 的其他方法
    // 直接转发到 ICE 发送
    
    if (!m_is_valid.load()) return -1;
    
    // 注意：这里不能加 m_mutex 锁，因为可能在 send() 中被调用，
    // 而 send() 已经持有锁，会导致死锁。
    // 但 m_ice 是 shared_ptr，线程安全。
    auto ice = m_ice;  // 拷贝一份，避免析构时的竞态
    if (ice) {
        return ice->send(data, len);
    }
    return -1;
}

void PeerController::on_kcp_message_received(const std::string& message) {
    // 此回调在 KcpSession::receive() 的锁外触发，是安全的
    
    if (!m_is_valid.load()) return;
    
    if (m_callbacks.on_message_received) {
        m_callbacks.on_message_received(message);
    }
}

// ═══════════════════════════════════════════════════════════════
// 设置 KCP 会话
// ═══════════════════════════════════════════════════════════════

void PeerController::setup_kcp_session() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 防止重复创建
    if (m_kcp) return;
    
    uint32_t conv = calculate_conv();
    
    KcpSessionCallbacks kcp_callbacks;
    
    // 注意：这里捕获 this 是安全的，因为：
    // 1. on_output 在 KCP 内部调用，我们不调用 KCP 方法，不会死锁
    // 2. m_kcp 的生命周期由 PeerController 管理
    kcp_callbacks.on_output = [this](const char* data, int len) -> int {
        return on_kcp_output(data, len);
    };
    
    kcp_callbacks.on_message_received = [this](const std::string& message) {
        on_kcp_message_received(message);
    };
    
    m_kcp = KcpSession::create(conv, std::move(kcp_callbacks));
    
    if (m_kcp && g_logger) {
        g_logger->info("[PeerController] KCP session created for {} <-> {} with conv {} (offer_side={})", 
                       m_self_id, m_peer_id, conv, m_is_offer_side);
    }
}

} // namespace VeritasSync
