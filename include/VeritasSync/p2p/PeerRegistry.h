#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VeritasSync {

// 前向声明
class PeerController;
enum class PeerState;

/// 线程安全的 Peer 容器，封装 m_peers + m_peers_mutex
/// 职责：仅管"存取"，不管业务逻辑
class PeerRegistry {
public:
    /// 添加 peer（写锁）
    void add(const std::string& peer_id, std::shared_ptr<PeerController> controller);

    /// 原子性"查找+添加"（写锁），避免 find+add 之间的 TOCTOU
    /// @return true 表示成功插入，false 表示已存在（未替换）
    bool try_add(const std::string& peer_id, std::shared_ptr<PeerController> controller);

    /// 移除 peer 并返回（写锁），不存在返回 nullptr
    std::shared_ptr<PeerController> remove(const std::string& peer_id);

    /// 按 peer_id 查找（读锁），不存在返回 nullptr
    std::shared_ptr<PeerController> find(const std::string& peer_id) const;

    /// 收集所有已连接的 PeerController（读锁内拷贝，锁外安全使用）
    std::vector<std::shared_ptr<PeerController>> collect_connected() const;

    /// 收集全部 PeerController 的 shared_ptr 拷贝（读锁内拷贝）
    std::vector<std::shared_ptr<PeerController>> collect_all() const;

    /// 在读锁内遍历所有 peer，对每个执行 callback
    /// callback 签名: void(const std::string& peer_id, const std::shared_ptr<PeerController>&)
    using ForEachCallback = std::function<void(const std::string&, const std::shared_ptr<PeerController>&)>;
    void for_each(ForEachCallback cb) const;

    /// 检查已存在的 peer 是否可复用（写锁）
    /// @return true 表示应跳过该 peer（已连接/正在连接且非强制）
    /// @return false 表示已清理旧的或不存在，应继续创建
    bool try_reuse_or_evict(const std::string& peer_id, bool force);

    /// 关闭并清空所有 peer（写锁）
    void close_all();

    /// 返回当前 peer 数量（读锁）
    size_t size() const;

private:
    std::unordered_map<std::string, std::shared_ptr<PeerController>> m_peers;
    mutable std::shared_mutex m_mutex;
};

}  // namespace VeritasSync
