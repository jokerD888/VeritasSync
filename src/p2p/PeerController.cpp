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
    std::shared_ptr<CryptoLayer> crypto,
    PeerControllerCallbacks callbacks,
    const KcpConfig& kcp_config) {
    
    // 使用内部派生类绕过 private 构造函数
    struct PeerControllerMaker : public PeerController {
        PeerControllerMaker(
            const std::string& self,
            const std::string& peer,
            boost::asio::io_context& ioc,
            std::shared_ptr<CryptoLayer> cry,
            PeerControllerCallbacks cb,
            const KcpConfig& kcp_cfg)
            : PeerController(self, peer, ioc, std::move(cry), std::move(cb), kcp_cfg) {}
    };

    auto controller = std::make_shared<PeerControllerMaker>(
        self_id, peer_id, io_context, std::move(crypto), std::move(callbacks), kcp_config);
    
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
    std::shared_ptr<CryptoLayer> crypto,
    PeerControllerCallbacks callbacks,
    const KcpConfig& kcp_config)
    : m_self_id(self_id)
    , m_peer_id(peer_id)
    , m_is_offer_side(self_id < peer_id)  // 自动判断角色
    , m_io_context(io_context)
    , m_crypto(std::move(crypto))
    , m_callbacks(std::move(callbacks))
    , m_kcp_config(kcp_config) {
}

PeerController::~PeerController() {
    // 确保资源被释放
    close();
}

// ═══════════════════════════════════════════════════════════════
// 初始化
// ═══════════════════════════════════════════════════════════════

bool PeerController::initialize_ice(const IceConfig& ice_config) {
    // 第一阶段：仅创建 IceTransport，回调留空
    // 回调将在 bind_callbacks() 中设置（此时 shared_ptr 已就绪）
    
    IceTransportCallbacks ice_callbacks;  // 空回调，后续在 bind_callbacks 中设置

    m_ice = IceTransport::create(ice_config, std::move(ice_callbacks), m_io_context);
    if (!m_ice) {
        if (g_logger) {
            g_logger->error("[PeerController] Failed to create IceTransport for {}", m_peer_id);
        }
        return false;
    }
    
    return true;
}

void PeerController::bind_callbacks() {
    // C-1 安全修复：ICE 回调改用 weak_ptr
    // 现在 shared_ptr 已创建，可以安全使用 weak_from_this()
    // weak_ptr::lock() 是原子判活 + 延寿，彻底消除 Use-After-Free
    
    auto self_weak = weak_from_this();
    auto& io = m_io_context;
    
    IceTransportCallbacks ice_callbacks;
    
    ice_callbacks.on_state_changed = [self_weak, &io](IceState state) {
        boost::asio::post(io, [self_weak, state]() {
            auto self = self_weak.lock();
            if (!self) return;  // 对象已析构，安全跳过
            self->on_ice_state_changed(state);
        });
    };
    
    ice_callbacks.on_local_candidate = [self_weak, &io](const std::string& candidate) {
        boost::asio::post(io, [self_weak, candidate]() {
            auto self = self_weak.lock();
            if (!self) return;
            self->on_ice_local_candidate(candidate);
        });
    };
    
    ice_callbacks.on_gathering_done = [self_weak, &io](const std::string& local_desc) {
        boost::asio::post(io, [self_weak, local_desc]() {
            auto self = self_weak.lock();
            if (!self) return;
            self->on_ice_gathering_done(local_desc);
        });
    };

    ice_callbacks.on_data_received = [self_weak, &io](const char* data, size_t size) {
        // 数据接收需要拷贝，因为原始指针在回调返回后可能无效
        std::string data_copy(data, size);
        boost::asio::post(io, [self_weak, data_copy]() {
            auto self = self_weak.lock();
            if (!self) return;
            self->on_ice_data_received(data_copy.data(), data_copy.size());
        });
    };

    m_ice->set_callbacks(std::move(ice_callbacks));
    
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
    
    if (!m_ice) return;
    
    m_state.store(PeerState::Connecting);
    m_ice->gather_candidates();
    
    if (g_logger) {
        g_logger->info("[PeerController] {} initiating connection to {} (offer_side={})",
                       m_self_id, m_peer_id, m_is_offer_side);
    }
}

void PeerController::handle_signaling(const std::string& signal_type, const std::string& payload) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_ice) return;
    
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
    // C-1: 标记为已关闭，is_valid() 返回 false（atomic，锁外安全）
    m_closed.store(true);

    // A-6 修复：状态变更与资源释放在同一把锁内完成
    // 防止其他线程在 m_state=Disconnected 但资源尚未释放的窗口期内访问已失效的指针
    std::lock_guard<std::mutex> lock(m_mutex);

    m_state.store(PeerState::Disconnected);

    // 取消同步超时定时器
    if (m_sync_timeout_timer) {
        m_sync_timeout_timer->cancel();
        m_sync_timeout_timer.reset();
    }

    // 释放资源
    m_kcp.reset();
    m_ice.reset();
    m_kcp_initialized.store(false);
}

// ═══════════════════════════════════════════════════════════════
// 消息收发
// ═══════════════════════════════════════════════════════════════

int PeerController::send_message(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_kcp) return -1;
    
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
    // 在锁内仅拷贝 KcpSession 的 shared_ptr
    std::shared_ptr<KcpSession> kcp;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_kcp) return;
        kcp = m_kcp;
    }
    // update() 内部会触发 on_kcp_output 回调（加密+网络I/O），不应持锁
    // update() 内部自动收取消息并触发 on_message_received 回调（锁外）
    kcp->update(current_ms);
}

uint32_t PeerController::check_kcp(uint32_t current_ms) const {
    std::shared_ptr<KcpSession> kcp;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_kcp) return current_ms;
        kcp = m_kcp;
    }
    return kcp->check(current_ms);
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

void PeerController::flush_kcp() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp && m_kcp->is_valid()) {
        m_kcp->flush();
    }
}

// ═══════════════════════════════════════════════════════════════
// ICE 回调处理
// ═══════════════════════════════════════════════════════════════

void PeerController::on_ice_state_changed(IceState state) {
    // 此方法通过 weak_ptr + post 调用，到达这里说明对象仍存活

    // 【P0-a 修复】已切换到中继模式后，忽略所有后续 ICE 状态变化
    // 防止 libjuice completed↔failed 循环不断触发回调（日志刷屏+重复 flood sync）
    if (m_relay_mode.load()) {
        if (g_logger) {
            g_logger->debug("[PeerController] {} 已在中继模式，忽略 ICE 状态变化: {}",
                           m_peer_id, static_cast<int>(state));
        }
        return;
    }

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
            m_connected_at_ts.store(
                std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count());
        }

        // 【P0-b 修复】仅在首次到达 Connected/Completed 时通知上层
        // 使用 compare_exchange 确保 Connecting→Connected 转换只发生一次
        // 防止 ICE Connected(3) 和 Completed(4) 重复触发 perform_flood_sync
        PeerState prev = PeerState::Connecting;
        if (m_state.compare_exchange_strong(prev, PeerState::Connected)) {
            if (m_callbacks.on_state_changed) {
                m_callbacks.on_state_changed(PeerState::Connected);
            }
        }
        // else: 已经是 Connected 状态（Completed 重复触发），静默跳过

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
    
    if (m_callbacks.on_signal_needed) {
        m_callbacks.on_signal_needed("ice_candidate", candidate);
    }
}

void PeerController::on_ice_gathering_done(const std::string& local_desc) {

    if (m_callbacks.on_signal_needed) {
        // 根据角色发送正确的信令类型（SDP Offer/Answer）
        std::string signal_type = m_is_offer_side ? "sdp_offer" : "sdp_answer";
        m_callbacks.on_signal_needed(signal_type, local_desc);

        // SDP 发送完毕后立即通知对端候选收集结束
        m_callbacks.on_signal_needed("ice_gathering_done", "");
    }
}

void PeerController::on_ice_data_received(const char* data, size_t size) {
    
    // 【加密层下沉】
    // 1. 尝试解密
    std::string ciphertext(data, size);
    std::string plaintext = m_crypto->decrypt(ciphertext);

    // 2. 解密失败 = 丢包 (KCP 会超时重传)
    // 这种机制确保了只有合法且完整的数据包才能进入 KCP 状态机
    if (plaintext.empty()) {
        if (g_logger) {
            g_logger->warn("[PeerController] ❌ 解密失败或 Tag 不匹配，丢弃数据包 ({} bytes)", size);
        }
        return;
    }

    // ICE 收到的数据喂给 KCP
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        // 3. 将明文喂给 KCP
        m_kcp->input(plaintext.data(), plaintext.size());
    }
}

// ═══════════════════════════════════════════════════════════════
// KCP 回调处理
// ═══════════════════════════════════════════════════════════════

int PeerController::on_kcp_output(const char* data, int len) {
    // 此回调在 KCP 内部触发，不能调用 KCP 的其他方法

    if (m_state.load() == PeerState::Disconnected) return -1;

    // 【加密层下沉】
    // 1. 加密整个 KCP 包 (Header + Content)
    std::string plaintext(data, len);
    std::string ciphertext = m_crypto->encrypt(plaintext);

    if (ciphertext.empty()) {
        if (g_logger) {
            g_logger->error("[PeerController] ❌ 加密失败，无法发送");
        }
        return -1;
    }

    // 2. 中继模式：通过 Tracker 服务器转发密文
    if (m_relay_mode.load()) {
        // 在锁内拷贝 relay_send 函数对象，锁外调用，避免数据竞争
        std::function<void(const std::string&, const uint8_t*, size_t)> relay_send;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            relay_send = m_relay_send;
        }
        if (relay_send) {
            relay_send(m_peer_id,
                reinterpret_cast<const uint8_t*>(ciphertext.data()),
                ciphertext.size());
            return 0;
        }
        return -1;
    }

    // 3. 直连模式：通过 ICE 发送密文
    // 注意：这里不能加 m_mutex 锁，因为可能在 send() 中被调用，
    // 而 send() 已经持有锁，会导致死锁。
    // 但 m_ice 是 shared_ptr，线程安全。
    auto ice = m_ice;  // 拷贝一份，避免析构时的竞态
    if (ice) {
        return ice->send(ciphertext.data(), ciphertext.size());
    }
    return -1;
}

void PeerController::on_kcp_message_received(const std::string& message) {
    // 此回调在 KcpSession::update() 的锁外触发，是安全的

    if (m_callbacks.on_message_received) {
        m_callbacks.on_message_received(message);
    }
}

// ═══════════════════════════════════════════════════════════════
// 中继模式（ICE 失败时的回退方案）
// ═══════════════════════════════════════════════════════════════

void PeerController::enable_relay_mode(
    std::function<void(const std::string& peer_id, const uint8_t* data, size_t len)> send_func) {

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // TOCTOU 修复：is_valid() 检查移入锁内，防止检查与状态变更之间的竞态
        if (m_closed.load()) {
            if (g_logger) g_logger->warn("[PeerController] enable_relay_mode 跳过: {} 已关闭", m_peer_id);
            return;
        }

        m_relay_send = std::move(send_func);
        m_relay_mode.store(true);  // 与 m_relay_send 在同一把锁内，保证 on_kcp_output 看到一致状态

        // 【P0-a 修复】销毁 ICE agent，终止 libjuice 的 completed↔failed 状态循环
        // relay 模式下数据走 Tracker 中转，ICE 通道不再需要
        // 注意：m_relay_mode 已设为 true，on_ice_state_changed 会忽略后续回调
        // 即使有已 post 到 io_context 的回调也会被 relay 检查拦截
        if (m_ice) {
            g_logger->info("[PeerController] {} 切换到中继模式，销毁 ICE agent", m_peer_id);
            m_ice.reset();  // → ~IceTransport() → juice_destroy()
        }
    }

    // 初始化 KCP（ICE 可能从未到达 Connected 状态，KCP 尚未创建）
    // 【修复致命死锁】不在此处加锁，setup_kcp_session() 内部会自行加锁
    // 原代码在此处持有 m_mutex 后调用 setup_kcp_session()，而后者也会加 m_mutex，
    // std::mutex 不可递归加锁（UB），Windows 上导致死锁/崩溃
    bool expected = false;
    if (m_kcp_initialized.compare_exchange_strong(expected, true)) {
        setup_kcp_session();
    }

    // 记录连接时间
    auto now = std::chrono::system_clock::now();
    m_connected_at_ts.store(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

    // 切换到 Connected 状态，延迟通知上层（避免在 handle_peer_state_changed(Failed) 中重入）
    m_state.store(PeerState::Connected);
    if (m_callbacks.on_state_changed) {
        auto cb = m_callbacks.on_state_changed;
        boost::asio::post(m_io_context, [cb]() {
            cb(PeerState::Connected);
        });
    }

    if (g_logger) {
        g_logger->info("[PeerController] {} 已切换到中继模式 (relay via tracker)", m_peer_id);
    }
}

void PeerController::feed_relay_data(const uint8_t* data, size_t len) {
    // 复用现有解密 → KCP input 路径
    on_ice_data_received(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
}

// ═══════════════════════════════════════════════════════════════
// 设置 KCP 会话
// ═══════════════════════════════════════════════════════════════

void PeerController::setup_kcp_session() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 防止重复创建，或在已关闭的 controller 上创建
    if (m_kcp || !is_valid()) return;
    
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
    
    m_kcp = KcpSession::create(conv, std::move(kcp_callbacks), m_kcp_config);
    
    if (m_kcp && g_logger) {
        g_logger->info("[PeerController] KCP session created for {} <-> {} with conv {} (offer_side={})", 
                       m_self_id, m_peer_id, conv, m_is_offer_side);
    }
}

// ═══════════════════════════════════════════════════════════════
// A-6: 同步超时定时器封装（线程安全）
// ═══════════════════════════════════════════════════════════════

void PeerController::start_sync_timeout(
    int timeout_seconds,
    std::function<void(const boost::system::error_code&)> callback) {
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 取消旧的定时器（如果有）
    if (m_sync_timeout_timer) {
        m_sync_timeout_timer->cancel();
    }
    
    m_sync_timeout_timer = std::make_shared<boost::asio::steady_timer>(m_io_context);
    m_sync_timeout_timer->expires_after(std::chrono::seconds(timeout_seconds));
    m_sync_timeout_timer->async_wait(std::move(callback));
}

bool PeerController::refresh_sync_timeout(uint64_t session_id, int timeout_seconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_sync_progress.session_id.load() == session_id && m_sync_timeout_timer) {
        m_sync_timeout_timer->expires_after(std::chrono::seconds(timeout_seconds));
        return true;
    }
    return false;
}

bool PeerController::has_sync_timeout_timer() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sync_timeout_timer != nullptr;
}

} // namespace VeritasSync
