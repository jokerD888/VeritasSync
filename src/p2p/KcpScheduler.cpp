#include "VeritasSync/p2p/KcpScheduler.h"

#include "VeritasSync/net/BinaryFrame.h"
#include "VeritasSync/p2p/PeerController.h"

#include <algorithm>

namespace VeritasSync {

KcpScheduler::KcpScheduler(boost::asio::io_context& io_context,
                           CollectPeersFunc collect_peers,
                           uint32_t initial_interval_ms)
    : m_io_context(io_context),
      m_timer(io_context),
      m_collect_peers(std::move(collect_peers)),
      m_interval_ms(initial_interval_ms),
      m_last_keepalive_time(std::chrono::steady_clock::now()) {
}

void KcpScheduler::start() {
    m_running.store(true);
    schedule_update();
}

void KcpScheduler::stop() {
    m_running.store(false);
    m_timer.cancel();
}

void KcpScheduler::schedule_update() {
    if (!m_running.load()) return;

    m_timer.expires_after(std::chrono::milliseconds(m_interval_ms.load()));
    m_timer.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && m_running.load()) {
            update_all_kcps();
        }
    });
}

void KcpScheduler::update_all_kcps() {
    auto now = std::chrono::steady_clock::now();
    auto current_time_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());

    // 先在锁外收集 controller 列表（回调安全）
    auto controllers = m_collect_peers();

    // 无活跃连接时使用最大间隔，跳过 KCP 驱动和保活检查
    if (controllers.empty()) {
        m_interval_ms.store(INTERVAL_MAX_MS);
        schedule_update();
        return;
    }

    // 驱动所有 KCP 并查询下次唤醒时间
    uint32_t next_wakeup = current_time_ms + INTERVAL_MAX_MS;

    for (auto& ctrl : controllers) {
        if (!ctrl->is_valid()) continue;

        ctrl->update_kcp(current_time_ms);

        // ikcp_check 返回 KCP 需要下次 update 的精确时间
        // 有重传/ACK pending 时返回近时间，空闲时返回远时间
        uint32_t kcp_next = ctrl->check_kcp(current_time_ms);
        next_wakeup = std::min(next_wakeup, kcp_next);
    }

    // 计算下次定时器间隔：clamp 到 [MIN, MAX]
    uint32_t elapsed_since_now = next_wakeup > current_time_ms
        ? next_wakeup - current_time_ms : 0;
    uint32_t interval = std::clamp(elapsed_since_now, INTERVAL_MIN_MS, INTERVAL_MAX_MS);

    m_interval_ms.store(interval);

    // NAT 保活心跳：无论是否有数据活动，定期发送 PING 保持映射存活
    auto since_last_keepalive = now - m_last_keepalive_time;
    if (since_last_keepalive >= std::chrono::seconds(KEEPALIVE_INTERVAL_SECONDS)) {
        m_last_keepalive_time = now;
        std::string ping_msg(1, static_cast<char>(MSG_TYPE_PING));
        for (auto& ctrl : controllers) {
            if (ctrl->is_valid() && ctrl->is_connected()) {
                ctrl->send_message(ping_msg);
            }
        }
    }

    schedule_update();
}

}  // namespace VeritasSync
