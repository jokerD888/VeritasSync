
#pragma once

#include <mutex>
#include <string>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

// --- miniupnpc 头文件 ---
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
// --------------------------

namespace VeritasSync {

/**
 * @brief UPnP 管理器
 * 
 * 负责 UPnP 设备发现、公网 IP 获取和端口映射。
 * 可以在 ICE 候选地址中将局域网 IP 替换为公网 IP，
 * 从而在支持 UPnP 的路由器上改善 NAT 穿透成功率。
 * 
 * 线程安全：所有方法都通过 m_mutex 保护，可安全地在多线程环境中使用。
 */
class UpnpManager {
public:
    UpnpManager();
    ~UpnpManager();

    // 禁止拷贝
    UpnpManager(const UpnpManager&) = delete;
    UpnpManager& operator=(const UpnpManager&) = delete;

    /**
     * @brief 在线程池中异步初始化 UPnP 发现
     * 
     * 发现路由器上的 UPnP 设备并获取公网 IP。
     * 此操作可能耗时 2-3 秒，因此在线程池中执行。
     * 
     * @param pool 工作线程池
     */
    void init_async(boost::asio::thread_pool& pool, int discover_timeout_ms = 2000);

    /**
     * @brief 尝试重写 ICE 候选地址（host 类型 → 公网 IP）
     * 
     * 对于 host 类型的 ICE 候选地址，如果其 IP 匹配 UPnP 发现的局域网 IP，
     * 则尝试在路由器上添加端口映射，并将候选地址中的 IP 替换为公网 IP。
     * 
     * 如果 UPnP 不可用或映射失败，返回原始候选地址。
     * 
     * @param sdp_candidate SDP 格式的 ICE 候选地址
     * @return 重写后的候选地址（或原始地址）
     */
    std::string rewrite_candidate(const std::string& sdp_candidate);

    /// 查询是否已成功发现 UPnP 设备并获取公网 IP
    bool is_available() const;

    /// 获取公网 IP（如果已发现）
    std::string get_public_ip() const;

private:
    mutable std::mutex m_mutex;
    bool m_available = false;
    char m_lan_addr[64] = {0};
    std::string m_public_ip;
    struct UPNPUrls m_urls;
    struct IGDdatas m_data;
    bool m_initialized = false;  // 标记 m_urls/m_data 是否已初始化（析构时需要清理）
};

}  // namespace VeritasSync
