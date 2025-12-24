#include "VeritasSync/p2p/PeerController.h"
#include "VeritasSync/common/Logger.h"

#include <functional>

namespace VeritasSync {

// --- 工厂方法 ---
std::shared_ptr<PeerController> PeerController::create(
    const std::string& peer_id,
    boost::asio::io_context& io_context,
    const IceConfig& ice_config,
    PeerControllerCallbacks callbacks) {
    
    struct PeerControllerMaker : public PeerController {
        PeerControllerMaker(const std::string& id, boost::asio::io_context& ioc, PeerControllerCallbacks cb)
            : PeerController(id, ioc, std::move(cb)) {}
    };
    
    auto controller = std::make_shared<PeerControllerMaker>(peer_id, io_context, std::move(callbacks));
    if (!controller->initialize(ice_config)) {
        return nullptr;
    }
    return controller;
}

// --- 构造与析构 ---
PeerController::PeerController(
    const std::string& peer_id,
    boost::asio::io_context& io_context,
    PeerControllerCallbacks callbacks)
    : m_peer_id(peer_id)
    , m_io_context(io_context)
    , m_callbacks(std::move(callbacks)) {
}

PeerController::~PeerController() {
    m_is_valid.store(false);
    // IceTransport 和 KcpSession 会在智能指针析构时自动清理
}

bool PeerController::initialize(const IceConfig& ice_config) {
    // 创建 ICE 传输层
    IceTransportCallbacks ice_callbacks;
    
    // 捕获 weak_ptr 避免循环引用
    std::weak_ptr<PeerController> weak_self;
    
    ice_callbacks.on_state_changed = [this](IceState state) {
        on_ice_state_changed(state);
    };
    
    ice_callbacks.on_local_candidate = [this](const std::string& candidate) {
        on_ice_local_candidate(candidate);
    };
    
    ice_callbacks.on_gathering_done = [this](const std::string& local_desc) {
        on_ice_gathering_done(local_desc);
    };
    
    ice_callbacks.on_data_received = [this](const char* data, size_t size) {
        on_ice_data_received(data, size);
    };
    
    m_ice = IceTransport::create(ice_config, std::move(ice_callbacks));
    if (!m_ice) {
        if (g_logger) g_logger->error("[PeerController] Failed to create IceTransport for {}", m_peer_id);
        return false;
    }
    
    return true;
}

// --- 连接控制 ---
void PeerController::initiate_connection() {
    if (m_ice) {
        m_state.store(PeerState::Connecting);
        m_ice->gather_candidates();
    }
}

void PeerController::handle_signaling(const std::string& signal_type, const std::string& payload) {
    if (!m_ice) return;
    
    if (signal_type == "ice_candidate") {
        m_ice->add_remote_candidate(payload);
    } else if (signal_type == "sdp_offer") {
        m_ice->set_remote_description(payload);
        // 收到 Offer 后开始收集候选
        m_state.store(PeerState::Connecting);
        m_ice->gather_candidates();
    } else if (signal_type == "sdp_answer") {
        m_ice->set_remote_description(payload);
    } else if (signal_type == "ice_gathering_done") {
        m_ice->set_remote_gathering_done();
    }
}

void PeerController::close() {
    m_is_valid.store(false);
    m_state.store(PeerState::Disconnected);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    m_kcp.reset();
    m_ice.reset();
}

// --- 消息收发 ---
int PeerController::send_message(const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_kcp || !m_is_valid.load()) return -1;
    
    m_kcp->send(data, size);
    return m_kcp->get_wait_send_count();
}

int PeerController::send_message(const std::string& message) {
    return send_message(message.data(), message.size());
}

// --- KCP 驱动 ---
void PeerController::update_kcp(uint32_t current_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        m_kcp->update(current_ms);
        
        // 接收消息
        auto messages = m_kcp->receive();
        // 注意：消息已通过 on_kcp_message_received 回调处理
    }
}

// --- 状态查询 ---
IceConnectionType PeerController::get_connection_type() const {
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

// --- ICE 回调处理 ---
void PeerController::on_ice_state_changed(IceState state) {
    if (g_logger) {
        g_logger->info("[PeerController] {} ICE state changed to {}", m_peer_id, static_cast<int>(state));
    }
    
    if (state == IceState::Connected || state == IceState::Completed) {
        // ICE 连接成功，设置 KCP
        if (!m_kcp) {
            // 生成 conv ID (两端一致的哈希)
            uint32_t conv = static_cast<uint32_t>(std::hash<std::string>{}(m_peer_id));
            setup_kcp_session(conv);
            
            // 记录连接时间
            auto now = std::chrono::system_clock::now();
            connected_at_ts = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
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
    if (m_callbacks.on_signal_needed) {
        m_callbacks.on_signal_needed("ice_candidate", candidate);
    }
}

void PeerController::on_ice_gathering_done(const std::string& local_desc) {
    if (m_callbacks.on_signal_needed) {
        // 根据连接角色发送 Offer 或 Answer
        // 这里简化为统一使用 local_desc
        m_callbacks.on_signal_needed("sdp", local_desc);
    }
}

void PeerController::on_ice_data_received(const char* data, size_t size) {
    // ICE 收到的数据喂给 KCP
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_kcp) {
        m_kcp->input(data, size);
    }
}

// --- KCP 回调处理 ---
int PeerController::on_kcp_output(const char* data, int len) {
    // KCP 要发送数据，通过 ICE 发出
    if (m_ice && m_is_valid.load()) {
        return m_ice->send(data, len);
    }
    return -1;
}

void PeerController::on_kcp_message_received(const std::string& message) {
    // 通知上层收到消息
    if (m_callbacks.on_message_received) {
        m_callbacks.on_message_received(message);
    }
}

// --- 设置 KCP 会话 ---
void PeerController::setup_kcp_session(uint32_t conv) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    KcpSessionCallbacks kcp_callbacks;
    
    kcp_callbacks.on_output = [this](const char* data, int len) -> int {
        return on_kcp_output(data, len);
    };
    
    kcp_callbacks.on_message_received = [this](const std::string& message) {
        on_kcp_message_received(message);
    };
    
    m_kcp = KcpSession::create(conv, std::move(kcp_callbacks));
    
    if (m_kcp && g_logger) {
        g_logger->info("[PeerController] KCP session created for {} with conv {}", m_peer_id, conv);
    }
}

} // namespace VeritasSync
