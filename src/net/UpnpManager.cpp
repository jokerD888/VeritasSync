#include "VeritasSync/net/UpnpManager.h"

#include <sstream>

#include "VeritasSync/common/Logger.h"

namespace VeritasSync {

// E-1: 魔数统一为命名常量（保留为默认值参考）
static constexpr int DEFAULT_UPNP_DISCOVER_TIMEOUT_MS = 2000;  // UPnP 设备发现超时（毫秒）

// ═══════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════

static std::string get_sdp_field(const std::string& sdp, int index) {
    std::istringstream ss(sdp);
    std::string token;
    for (int i = 0; i <= index && std::getline(ss, token, ' '); ++i) {
        if (i == index) return token;
    }
    return "";
}

// ═══════════════════════════════════════════════════════════════
// 构造 / 析构
// ═══════════════════════════════════════════════════════════════

UpnpManager::UpnpManager() {
    std::memset(&m_urls, 0, sizeof(m_urls));
    std::memset(&m_data, 0, sizeof(m_data));
}

UpnpManager::~UpnpManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) {
        FreeUPNPUrls(&m_urls);
    }
}

// ═══════════════════════════════════════════════════════════════
// 异步初始化
// ═══════════════════════════════════════════════════════════════

void UpnpManager::init_async(boost::asio::thread_pool& pool, int discover_timeout_ms) {
    boost::asio::post(pool, [this, discover_timeout_ms]() {
        int error = 0;
        // 发现路由器 (超时由配置决定)
        struct UPNPDev* devlist = upnpDiscover(discover_timeout_ms, nullptr, nullptr, 0, 0, 2, &error);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (devlist) {
            g_logger->info("[UPnP] 发现 UPnP 设备列表。");

            // 获取有效的 IGD (互联网网关设备)
            char wanaddr[64] = {0};
            int r = UPNP_GetValidIGD(devlist, &m_urls, &m_data,
                                     m_lan_addr, sizeof(m_lan_addr),
                                     wanaddr, sizeof(wanaddr));

            if (r == 1) {
                m_initialized = true;  // 标记已初始化（析构时需要 FreeUPNPUrls）
                g_logger->info("[UPnP] 成功连接到路由器: {}", m_urls.controlURL);
                g_logger->info("[UPnP] 我们的局域网 IP: {}", m_lan_addr);

                // 获取公网 IP
                char public_ip[40];
                r = UPNP_GetExternalIPAddress(m_urls.controlURL,
                                              m_data.first.servicetype, public_ip);

                if (r == UPNPCOMMAND_SUCCESS) {
                    m_public_ip = public_ip;
                    m_available = true;
                    g_logger->info("[UPnP] ✅ 成功获取公网 IP: {}", m_public_ip);
                } else {
                    g_logger->warn("[UPnP] 无法获取公网 IP (错误码: {}).", r);
                }
            } else {
                g_logger->warn("[UPnP] 未找到有效的 IGD (互联网网关设备).");
            }
            freeUPNPDevlist(devlist);
        } else {
            g_logger->warn("[UPnP] 未发现 UPnP 设备 (错误: {}).", error);
        }
    });
}

// ═══════════════════════════════════════════════════════════════
// 候选地址重写
// ═══════════════════════════════════════════════════════════════

std::string UpnpManager::rewrite_candidate(const std::string& sdp_candidate) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 如果 UPnP 不可用，或者我们没有公网IP，则不重写
    if (!m_available || m_public_ip.empty()) {
        return sdp_candidate;
    }

    // libjuice 的候选地址格式: "a=candidate:..."
    // 我们只关心 "host" 类型的候选地址，它们包含局域网IP
    std::string cand_type = get_sdp_field(sdp_candidate, 7);
    if (cand_type != "host") {
        return sdp_candidate;  // 不是 "host"，可能是 "srflx" 或 "relay"，直接返回
    }

    // "a=candidate:..." 字段: 4=ip, 5=port
    std::string local_ip = get_sdp_field(sdp_candidate, 4);
    std::string local_port = get_sdp_field(sdp_candidate, 5);

    // 确保是我们自己的局域网 IP
    if (local_ip != m_lan_addr) {
        g_logger->debug("[UPnP] 候选 IP {} 与 UPnP 局域网 IP {} 不匹配，跳过。", local_ip, m_lan_addr);
        return sdp_candidate;
    }

    // 尝试在路由器上添加这个端口映射
    // (将 公网端口 映射到 局域网IP:局域网端口)
    int r = UPNP_AddPortMapping(m_urls.controlURL, m_data.first.servicetype,
                                local_port.c_str(),  // external_port (使用与内部相同的端口)
                                local_port.c_str(),  // internal_port
                                m_lan_addr,          // internal_client
                                "VeritasSync P2P",   // description
                                "UDP",               // protocol
                                nullptr, "0");       // remote_host, duration

    if (r == UPNPCOMMAND_SUCCESS) {
        g_logger->info("[UPnP] 成功为候选地址 {}:{} 映射公网端口 {}", local_ip, local_port, local_port);

        // 成功！现在重写候选地址，用公网IP替换局域网IP
        std::string rewritten_candidate = sdp_candidate;
        size_t pos = rewritten_candidate.find(local_ip);
        if (pos != std::string::npos) {
            rewritten_candidate.replace(pos, local_ip.length(), m_public_ip);
            g_logger->info("[UPnP] 重写候选地址为: {}...", rewritten_candidate.substr(0, 40));
            return rewritten_candidate;
        }
    } else {
        g_logger->warn("[UPnP] 无法为 {}:{} 映射端口 (错误码: {}).", local_ip, local_port, r);
    }

    // 映射失败，返回原始候选地址
    return sdp_candidate;
}

// ═══════════════════════════════════════════════════════════════
// 查询方法
// ═══════════════════════════════════════════════════════════════

bool UpnpManager::is_available() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_available;
}

std::string UpnpManager::get_public_ip() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_public_ip;
}

}  // namespace VeritasSync
