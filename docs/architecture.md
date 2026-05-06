# VeritasSync 架构文档

## 目录

1. [系统概述](#1-系统概述)
2. [分层架构](#2-分层架构)
3. [核心组件](#3-核心组件)
4. [组件间关系与依赖](#4-组件间关系与依赖)
5. [回调链与依赖注入](#5-回调链与依赖注入)
6. [消息路由与协议](#6-消息路由与协议)
7. [完整工作流](#7-完整工作流)
8. [生命周期管理](#8-生命周期管理)
9. [并发模型与线程安全](#9-并发模型与线程安全)
10. [设计模式总结](#10-设计模式总结)

---

## 1. 系统概述

VeritasSync 是一个 C++20 实现的点对点文件同步系统。每个运行实例可以托管一个或多个独立的同步任务（SyncNode）；每个节点通过 TCP 连接到中心化的 Tracker 服务器进行对等发现，然后通过 ICE/UDP + KCP 实现可靠的点对点通信，使用 AES-256-GCM 加密。

### 核心特性

- **NAT 穿透**：基于 libjuice 的 ICE 协议（STUN/TURN）
- **可靠传输**：KCP 协议提供有序、可靠的 UDP 传输
- **端到端加密**：AES-256-GCM，密钥由 sync_key 通过 HKDF-SHA256 派生
- **断点续传**：Bitmap 追踪 + 数据库持久化
- **冲突检测**：三方合并（base/local/remote hash 对比）
- **最终一致性**：Anti-entropy 定时器保证即使丢包也能收敛

---

## 2. 分层架构

```
┌─────────────────────────────────────────────────────────┐
│  应用层                                                  │
│  main.cpp · WebUI (HTTPS/httplib) · NLFilterGenerator   │
├─────────────────────────────────────────────────────────┤
│  协调层                                                  │
│  SyncNode (每任务一个实例) · TrackerClient (TCP 信令)    │
├─────────────────────────────────────────────────────────┤
│  P2P 编排层                                              │
│  P2PManager · PeerRegistry · KcpScheduler               │
│  BroadcastManager · MessageRouter                       │
├─────────────────────────────────────────────────────────┤
│  同步业务逻辑层                                          │
│  SyncSession · SyncHandler · SyncManager                │
│  TransferManager                                        │
├─────────────────────────────────────────────────────────┤
│  存储/状态层                                             │
│  StateManager · CachedFileStore · Database              │
│  FileFilter · NLFilterGenerator                         │
├─────────────────────────────────────────────────────────┤
│  单连接管理层                                            │
│  PeerController (ICE + KCP + Crypto)                    │
├─────────────────────────────────────────────────────────┤
│  传输层                                                  │
│  IceTransport (libjuice) · KcpSession                   │
│  CryptoLayer · UpnpManager                              │
├─────────────────────────────────────────────────────────┤
│  信令基础设施                                            │
│  TrackerServer (独立二进制)                              │
└─────────────────────────────────────────────────────────┘
```

---

## 3. 核心组件

### 3.1 SyncNode — 任务协调器

每个同步任务的顶层容器，负责创建和管理所有子组件的生命周期。

| 职责 | 说明 |
|------|------|
| 组件创建 | 按正确顺序创建 P2PManager、TrackerClient、StateManager |
| 生命周期 | 通过 `std::once_flag` 保证 `stop()` 幂等 |
| 依赖注入 | 将各组件通过 raw pointer 和 callback 互相连接 |

### 3.2 P2PManager — 中央编排器

系统中最复杂的组件，拥有或持有所有运行时子组件的引用。

**拥有的子组件：**

```
P2PManager
├── boost::asio::io_context         (本任务的事件循环)
├── boost::asio::thread_pool        (工作线程池)
├── std::jthread io_thread          (运行 io_context::run())
├── CryptoLayer                     (AES-256-GCM)
├── PeerRegistry                    (线程安全的 peer 容器)
├── KcpScheduler                    (自适应 KCP 更新循环)
├── BroadcastManager                (全量同步 + 增量广播)
├── MessageRouter                   (消息分发表)
├── TransferManager                 (文件块 I/O)
├── SyncHandler                     (同步业务逻辑)
├── SyncSession                     (同步握手协议)
└── steady_timer                    (Peer 清理定时器)
```

**外部持有的 raw pointer（不拥有，生命周期由 SyncNode 保证）：**

```
StateManager*    m_state_manager
TrackerClient*   m_tracker_client
```

### 3.3 PeerController — 单连接管理

管理与一个对端的完整连接生命周期。组合 IceTransport + KcpSession + CryptoLayer。

**状态机：**
```
Disconnected → Connecting → Connected → Failed
                                ↑
                          (可 reset 重建)
```

**关键决策（构造时确定）：**
- Offer/Answer 角色：`self_id < peer_id` → 本端发 Offer
- KCP conv_id：由 peer_id 的字典序比较决定，双方无需协商

### 3.4 TransferManager — 文件传输引擎

负责文件的分块发送和接收，支持断点续传。

**关键数据结构：**
```cpp
struct ReceivingFile {
    std::mutex file_mutex;              // per-file 锁
    std::ofstream file_stream;
    std::string temp_path;
    uint32_t total_chunks;
    std::atomic<uint32_t> received_chunks;
    std::vector<bool> received_bitmap;  // chunk 去重
    std::string expected_hash;          // 完整性校验
    std::atomic<bool> busy;             // 防止并发清理
};
```

**并发策略：**
- `m_transfer_mutex`：保护 map 结构（短暂持有）
- `ReceivingFile::file_mutex`：保护单文件 I/O（长时间持有）
- `atomic<bool> busy`：防止 cleanup/cancel 与 handle_chunk 并发

### 3.5 StateManager — 文件状态中心

连接文件系统和 P2P 层的枢纽。

**变更管道：**
```
efsw 文件系统通知 (OS 线程)
    → notify_change_detected() (加入 pending 队列)
    → debounce_timer (5000ms，每次变更重置)
    → scan_and_hash_changes() (worker_pool 线程)
    → commit_changes() (io_context 线程)
    → StateManagerCallbacks (通知 P2PManager 广播)
```

**回声抑制（Echo Suppression）：**
```cpp
void mark_file_received(path, hash);      // 标记刚从对端收到的文件
bool check_and_clear_echo(path, hash);    // 文件监控检测到变更时调用
// 若文件是刚刚接收的，返回 true → 跳过广播 → 防止循环
```

### 3.6 BroadcastManager — 广播与反熵

| 职责 | 触发条件 |
|------|---------|
| 全量 Flood Sync | 新 peer 连接 / Anti-entropy 定时器到期 |
| 增量广播 | StateManager 通知文件变更 |
| Anti-entropy | 30秒静默定时器（每次发消息重置） |
| Goodbye 广播 | 优雅关闭 |

**背压驱动的批量发送：**
```
发送一批 → 注册 on_send_ready 回调 → KCP 队列排空时触发下一批
```

### 3.7 SyncSession — 同步握手

管理 `sync_begin` / `sync_ack` 的握手协议，追踪每个 peer 的同步进度。

```cpp
struct SyncProgress {
    uint64_t session_id;
    std::atomic<size_t> expected_files;
    std::atomic<size_t> expected_dirs;
    std::atomic<size_t> received_files;
    std::atomic<size_t> received_dirs;
};
```

### 3.8 SyncHandler — 同步业务逻辑

处理所有同步相关的协议消息，执行状态比对、冲突检测和文件请求。

### 3.9 SyncManager — 纯算法工具

无状态的静态工具类，提供文件/目录比对算法和冲突检测逻辑。

**执行顺序约束**（调用方必须遵循）：
1. `dirs_to_create` — 确保父目录存在再下载文件
2. `files_to_conflict_rename` — 将冲突本地文件移走
3. Download `files_to_request`
4. `files_to_delete` — 删除远端已无的文件（仅 OneWay）
5. `dirs_to_delete` — 删除远端已无的目录（仅 OneWay）

**冲突检测逻辑**：
- 无 base hash（首次同步或历史丢失）→ 视为冲突
- `local_hash == base_hash` → 仅远端修改 → 下载远端（无冲突）
- `remote_hash == base_hash` → 仅本地修改 → 保留本地（无冲突）
- 两者均不同于 base → 真冲突 → 重命名本地为 `.conflict.{timestamp}`，下载远端

---

## 4. 组件间关系与依赖

### 4.1 组件依赖图

```
SyncNode
│
├─── TrackerClient ──────────────────── TCP → TrackerServer
│        │
│        └── P2PManager (raw ptr)
│
├─── P2PManager
│        │
│        ├── PeerRegistry ─── [PeerController] × N
│        │                         │
│        │                         ├── IceTransport
│        │                         ├── KcpSession
│        │                         └── CryptoLayer
│        │
│        ├── KcpScheduler ─── PeerRegistry.collect_connected()
│        │
│        ├── MessageRouter ─── dispatch table
│        │
│        ├── BroadcastManager
│        │       │
│        │       ├── StateManager (raw ptr, 读取文件列表)
│        │       └── send_to_peer callback → PeerController
│        │
│        ├── TransferManager
│        │       │
│        │       ├── StateManager (raw ptr, 记录同步状态)
│        │       └── SendCallback → PeerController::send_message
│        │
│        ├── SyncHandler
│        │       │
│        │       ├── StateManager (raw ptr, 读取/比对状态)
│        │       ├── TransferManager (shared_ptr, 预注册/续传)
│        │       ├── send_to_peer callback
│        │       └── send_to_peer_safe callback
│        │
│        └── SyncSession
│                │
│                ├── StateManager (raw ptr)
│                └── send_to_peer callback
│
└─── StateManager
         │
         ├── Database (SQLite)
         ├── CachedFileStore (内存缓存)
         ├── FileFilter (.veritasignore)
         └── efsw (文件监控)
```

### 4.2 通信方向

| 方向 | 机制 | 示例 |
|------|------|------|
| P2PManager → 子组件 | 直接方法调用 | `m_sync_handler->handle_file_update(...)` |
| 子组件 → P2PManager | weak_ptr callback | `send_to_peer_safe(msg, peer_id)` |
| StateManager → P2PManager | callback (注入时设置) | `on_file_updates(files)` → 触发广播 |
| P2PManager → StateManager | raw ptr 直接调用 | `m_state_manager->get_all_files()` |
| TrackerClient ↔ P2PManager | 双向 raw ptr | 信令转发、peer 发现通知 |

---

## 5. 回调链与依赖注入

### 5.1 回调类型定义

```cpp
// PeerController 级别
struct PeerControllerCallbacks {
    std::function<void(PeerController*, PeerState)>       on_state_changed;
    std::function<void(const std::string& sdp, bool)>    on_signal_needed;
    std::function<void(const std::string&, PeerController*)> on_message_received;
};

// SyncHandler 使用的回调
using SendToPeerFunc     = std::function<void(const std::string& msg, PeerController* peer)>;
using SendToPeerSafeFunc = std::function<void(const std::string& msg, const std::string& peer_id)>;
using WithPeerFunc       = std::function<void(const std::string& peer_id,
                                              std::function<void(PeerController*)>)>;

// SyncSession 使用的回调
using GetPeerFunc    = std::function<std::shared_ptr<PeerController>(const std::string& peer_id)>;
using ResyncCallback = std::function<void(std::shared_ptr<PeerController>, uint64_t session_id)>;

// TransferManager 使用的回调
using SendCallback = std::function<int(const std::string& peer_id, const std::string& data)>;

// StateManager 使用的回调
struct StateManagerCallbacks {
    std::function<void(std::vector<FileInfo>)>    on_file_updates;
    std::function<void(std::vector<std::string>)> on_file_deletes;
    std::function<void(std::vector<DirEvent>)>    on_dir_changes;
};
```

### 5.2 回调注入时机（P2PManager 构造时）

```cpp
// P2PManager::create() 内部：

auto weak_self = weak_from_this();

// SyncHandler 的发送回调（裸指针版 — 同步上下文使用）
auto send_to_peer_cb = [weak_self](const std::string& msg, PeerController* peer) {
    auto self = weak_self.lock();
    if (!self) return;
    self->send_over_kcp_peer(msg, peer);
};

// SyncHandler 的发送回调（peer_id 版 — 异步上下文使用）
auto send_to_peer_safe_cb = [weak_self](const std::string& msg, const std::string& peer_id) {
    auto self = weak_self.lock();
    if (!self) return;
    self->send_over_kcp_peer_safe(msg, peer_id);
};

// TransferManager 的发送回调
SendCallback transfer_send_cb = [weak_self](const std::string& peer_id,
                                            const std::string& data) -> int {
    auto self = weak_self.lock();
    if (!self) return -1;
    auto ctrl = self->m_peer_registry.find(peer_id);
    if (!ctrl) return -1;
    return ctrl->send_message(data);  // 0x80 前缀 + 加密
};
```

### 5.3 两个 send 回调的区别

| 函数 | 参数 | 使用场景 |
|------|------|---------|
| `send_over_kcp_peer(msg, PeerController*)` | 裸指针 | 同步上下文，调用方已持有有效指针 |
| `send_over_kcp_peer_safe(msg, peer_id)` | peer_id 字符串 | 异步回调中，指针可能已失效 |

**使用原则：**
- 在 `handle_kcp_message` 的直接处理链中 → 用裸指针版（指针在作用域内保证有效）
- 在 `on_send_ready`、定时器回调等延迟执行的 lambda 中 → 用 peer_id 版（通过 registry 重新查找）

### 5.4 `set_state_manager()` 级联

```cpp
void P2PManager::set_state_manager(StateManager* sm) {
    m_state_manager = sm;
    m_transfer_manager->set_state_manager(sm);
    m_sync_handler->set_state_manager(sm);
    m_sync_session->set_state_manager(sm);
}
```

设置为 `nullptr`（关闭时）自动传播到所有子组件，各组件在操作前检查 `m_state_manager != nullptr`。

### 5.5 完整回调链示例：文件变更 → 远程同步

```
[文件系统变更]
    │
    ▼
efsw 通知 → StateManager::notify_change_detected()
    │
    ▼ (debounce 5000ms)
    │
StateManager::scan_and_hash_changes() [worker_pool 线程]
    │
    ▼ (post 到 io_context)
    │
StateManager::commit_changes()
    │
    ▼ on_file_updates callback
    │
P2PManager::broadcast_file_updates(files) [io_context 线程]
    │
    ▼
BroadcastManager::broadcast_file_updates()
    │
    ├─── PeerController A: send_to_peer(msg, ctrl_A)
    │         │
    │         ▼
    │    PeerController::send_message(json_msg)
    │         │ 前缀 0x01 + CryptoLayer::encrypt()
    │         ▼
    │    KcpSession::send(encrypted)
    │         │ KCP 分段
    │         ▼
    │    IceTransport::send(udp_bytes) → 网络
    │
    └─── PeerController B: send_to_peer(msg, ctrl_B) → ...
```

---

## 6. 消息路由与协议

### 6.1 KCP 层消息前缀

| 前缀字节 | 含义 | 处理方式 |
|----------|------|---------|
| `0x01` | JSON 协议消息 | 解密 → JSON 解析 → MessageRouter 分发 |
| `0x80` | 二进制文件块 | 解密 → TransferManager::handle_chunk |
| `0x03` | Ping 心跳 | 无进一步处理 |

### 6.2 JSON 协议消息类型

| 消息类型 | 方向 | 用途 |
|---------|------|------|
| `share_state` | Source → Dest | 全量状态列表（文件+目录） |
| `request_file` | Dest → Source | 请求文件传输（可带 start_chunk 续传） |
| `file_update` | 任一方向 | 增量：单文件变更 |
| `file_update_batch` | 任一方向 | 批量文件变更 |
| `file_delete` | 任一方向 | 增量：单文件删除 |
| `file_delete_batch` | 任一方向 | 批量文件删除 |
| `dir_create` | 任一方向 | 目录创建 |
| `dir_delete` | 任一方向 | 目录删除 |
| `dir_batch` | 任一方向 | 批量目录操作 |
| `sync_begin` | Source → Dest | Flood sync 开始（声明预期数量） |
| `sync_ack` | Dest → Source | Flood sync 确认（报告实收数量） |
| `goodbye` | 任一方向 | 优雅断开通知 |

### 6.3 MessageRouter 分发表

在 P2PManager 构造时注册：

| 消息类型 | Handler | receive_only |
|---------|---------|-------------|
| `share_state` | SyncHandler::handle_share_state | true |
| `file_update` | SyncHandler::handle_file_update | true |
| `file_update_batch` | SyncHandler::handle_file_update_batch | true |
| `file_delete` | SyncHandler::handle_file_delete | true |
| `file_delete_batch` | SyncHandler::handle_file_delete_batch | true |
| `dir_create` | SyncHandler::handle_dir_create | true |
| `dir_delete` | SyncHandler::handle_dir_delete | true |
| `dir_batch` | SyncHandler::handle_dir_batch | true |
| `request_file` | TransferManager::queue_upload | false |
| `sync_begin` | SyncSession::handle_sync_begin | true |
| `sync_ack` | SyncSession::handle_sync_ack | false |
| `goodbye` | P2PManager 内部处理 | false |

`receive_only=true` 的消息只在 Destination 角色（或双向模式）下分发。

### 6.4 消息入站全链路

```
[远程 Peer]
    │ UDP 数据包
    ▼
IceTransport::on_data_received (post 到 io_context)
    │
    ▼
KcpSession::feed_incoming(raw_bytes)
    │ KCP 重组 → on_message_received 回调触发
    ▼
PeerController::on_kcp_message(raw_msg)
    │ 读取 1 字节前缀：
    │   0x01 → CryptoLayer::decrypt() → JSON parse → on_message_received 回调
    │   0x80 → CryptoLayer::decrypt() → TransferManager::handle_chunk (二进制)
    │   0x03 → ping, 不进一步分发
    ▼
P2PManager::handle_kcp_message(json_msg, PeerController*)
    │ 提取 "type" 字段
    │ refresh_sync_timeout()
    │ MessageRouter::dispatch(type, payload, peer, can_receive)
    ▼
[对应 Handler]
    SyncHandler::handle_file_update / handle_share_state / ...
    TransferManager::queue_upload
    SyncSession::handle_sync_begin / handle_sync_ack
```

### 6.5 信令协议（Tracker TCP）

4字节大端长度前缀 + JSON 正文。

| 信令 | 流向 | 用途 |
|------|------|------|
| `REGISTER` | Client → Tracker | 加入 sync_key 房间 |
| `REG_ACK` | Tracker → Client | 已加入，返回现有 peer 列表 |
| `PEER_JOIN` | Tracker → Client | 新 peer 加入同一房间 |
| `PEER_LEAVE` | Tracker → Client | Peer 离开 |
| `SIGNAL` | Client ↔ Tracker ↔ Client | ICE offer/answer/candidate 中继 |

---

## 7. 完整工作流

### 7.1 Phase 0: Peer 发现

```
SyncNode::start()
    └─ TrackerClient::connect(sync_key)
        └─ TCP → REGISTER
        └─ REG_ACK 返回 [existing_peer_ids]
        └─ on_ready_callback → P2PManager::connect_to_peers(peer_list)

    [后续] PEER_JOIN 通知
        └─ P2PManager::on_peer_joined(new_peer_id)
```

### 7.2 Phase 1: ICE 连接建立

```
P2PManager::connect_to_peer(peer_id)
    └─ PeerRegistry::try_add(peer_id, new PeerController)
    └─ PeerController::start_connection()

    若 self_id < peer_id（Offer 方）:
        IceTransport 收集候选 → on_gathering_done
        → 构建 SDP Offer → on_signal_needed
        → TrackerClient::send_signaling_message(SIGNAL, offer_sdp)

    远端收到 SIGNAL(offer):
        PeerController::handle_remote_description(offer_sdp)
        IceTransport 创建 Answer → on_signal_needed
        → TrackerClient::send_signaling_message(SIGNAL, answer_sdp)

    Offer 端收到 answer → ICE 连通性检查 → CONNECTED 状态
    on_state_changed(Connected) → P2PManager::on_peer_connected()
```

### 7.3 Phase 2: 初始 Flood Sync

对端连接成功后立即触发：

```
P2PManager::on_peer_connected(controller)
    └─ (Source 或 BiDirectional 角色)
    └─ BroadcastManager::perform_flood_sync(controller, session_id)
        └─ SyncSession::send_sync_begin(peer, session_id, file_count, dir_count)
        └─ 分批发送 share_state：
            {
                "type": "share_state",
                "session_id": <uint64>,
                "files": [FileInfo, ...],
                "dirs": [relative_path, ...]
            }
        └─ 使用背压机制：每批发完等待 on_send_ready 再发下一批
```

### 7.4 Phase 3: 状态比对（Destination 端）

```
SyncHandler::handle_share_state(payload, from_peer)
    │
    ├─ 反序列化远程 FileInfo 列表
    ├─ 从 StateManager 加载本地 FileInfo 列表
    │
    ├─ SyncManager::compare_states_and_get_requests(local, remote)
    │   → SyncActions {
    │       files_to_request,         // 需要下载的文件
    │       files_to_delete,          // 需要删除的文件（OneWay 模式）
    │       files_to_conflict_rename  // 冲突文件
    │     }
    │   → DirSyncActions {
    │       dirs_to_create,
    │       dirs_to_delete
    │     }
    │
    ├─ sync_directory_actions()       // 先处理目录
    │
    ├─ resolve_conflict() 对每个冲突文件:
    │   ├─ 查找 base_hash（DB SyncHistory）
    │   ├─ 两端都修改且不同 → 重命名本地为 .conflict.{timestamp}
    │   └─ 返回 ConflictResult::{RequestRemote, Skip, NoAction}
    │
    └─ pace_and_send_files()          // 背压驱动的文件请求循环
        └─ 每发完一个 request_file → 注册 on_send_ready → 发下一个
```

### 7.5 Phase 4: 文件传输

**发送端：**
```
TransferManager::queue_upload(peer_id, request_payload)
    └─ Worker 线程:
        ├─ 校验路径安全（防止路径遍历攻击）
        ├─ 打开文件
        ├─ 若请求携带 start_chunk > 0：校验 hash/size → 续传
        ├─ for chunk in [start_chunk .. total_chunks):
        │   ├─ 读取 CHUNK_DATA_SIZE 字节
        │   ├─ Snappy 压缩
        │   └─ 构建二进制帧:
        │       [uint16 path_len][path][uint32 chunk_idx][uint32 total]
        │       [uint32 uncompressed_len][compressed_data]
        │   └─ SendCallback(peer_id, frame)
        │       → PeerController::send_message(0x80 + encrypted)
        └─ 背压控制：KCP pending > congestion_threshold → 等待 drain
```

**接收端：**
```
TransferManager::handle_chunk(decrypted_payload, peer_id)
    └─ Worker 线程:
        ├─ parse_chunk_payload() 解析头部 + Snappy 解压
        ├─ lookup_or_create_receiving(path, total_chunks, peer_id)
        │   ├─ [新任务] 创建完整的 ReceivingFile
        │   └─ [预注册] 补全占位记录的 temp_path/bitmap
        ├─ per-file 锁内:
        │   ├─ bitmap 去重检查
        │   ├─ seekp(chunk_index * CHUNK_SIZE) + write
        │   └─ bitmap[chunk_index] = true; received_chunks++
        └─ if received_chunks == total_chunks:
            └─ finalize_received_file():
                ├─ close stream
                ├─ SHA-256 校验 (与 expected_hash 比对)
                ├─ rename(temp → final)
                ├─ StateManager::record_sync_success()
                └─ StateManager::mark_file_received() (回声抑制)
```

### 7.6 Phase 5: 同步完成握手

```
Flood sync 发送端发送完毕后:
    SyncSession::send_sync_ack(peer, session_id, sent_files, sent_dirs)

接收端 (handle_sync_ack):
    ├─ 比较实收 vs 预期（PeerController::SyncProgress）
    ├─ 若 received < expected:
    │   └─ ResyncCallback(controller, session_id) // 重新 flood sync
    └─ 否则: 同步完成
```

### 7.7 Phase 6: 增量同步（持续运行）

```
文件变更 → efsw 通知
    → StateManager debounce (5000ms)
    → scan + hash
    → on_file_updates 回调
    → BroadcastManager::broadcast_file_updates()
        ├─ 小批量: 直接发 file_update / file_update_batch
        └─ 大批量: pace_and_send_file_batches() (背压驱动)

远端收到:
    SyncHandler::handle_file_update()
        → 检查是否需要下载 (hash 不同)
        → build_file_request() (含续传检查)
        → register_expected_metadata() (预注册元数据)
        → send request_file
```

### 7.8 Phase 7: Anti-Entropy 兜底

```
BroadcastManager: 30 秒静默定时器
    └─ 每次发消息时重置
    └─ 到期时 → 对所有已连接 peer 执行 perform_flood_sync()
    └─ 保证即使增量消息丢失，最终也能收敛
```

### 7.9 Phase 8: 优雅断开

```
SyncNode::stop()
    └─ BroadcastManager::broadcast_goodbye()
        └─ 对每个 peer:
            ├─ send {"type": "goodbye"}
            └─ PeerController::flush_kcp() // 强制立即刷出

远端收到 goodbye:
    └─ TransferManager::cancel_receives_for_peer(peer_id)
    └─ TransferManager::cancel_sends_for_peer(peer_id)
    └─ PeerController 标记为 Disconnected
    └─ PeerRegistry 清除
```

---

## 8. 生命周期管理

### 8.1 启动顺序（SyncNode::start）

必须按顺序执行，因为存在 raw pointer 依赖：

| Step | 操作 | 依赖 |
|------|------|------|
| 1 | 验证 sync_key, sync_folder, role | — |
| 2 | `create_directories(sync_folder)` | — |
| 3 | `P2PManager::create(config)` | — |
| 4 | `TrackerClient::create(p2p->get_io_context())` | Step 3 |
| 5 | 交叉设置 raw pointer: tracker ↔ p2p | Step 3, 4 |
| 6 | 设置角色、加密密钥、模式 | Step 3 |
| 7 | `StateManager::create(callbacks)` | Step 3 (weak_ptr) |
| 8 | `p2p->set_state_manager(sm)` 级联 | Step 3, 7 |
| 9 | `state_manager->scan_directory()` | Step 7 |
| 10 | `tracker->connect()` 开始发现 peer | Step 3, 4, 5 |

### 8.2 关闭顺序（SyncNode::stop）

顺序重要——避免 async 回调中访问已释放内存：

| Step | 操作 | 原因 |
|------|------|------|
| 1 | `p2p->shutdown_gracefully()` | 广播 goodbye + flush + join 所有线程 |
| 2 | `tracker->stop()` | 关闭 TCP |
| 3 | `tracker→p2p = nullptr` | 防止 use-after-free |
| 4 | `p2p->set_state_manager(nullptr)` | 级联置空所有子组件 |
| 5 | `p2p→tracker = nullptr` | 同上 |
| 6 | reset unique_ptrs | 析构按反向创建顺序 |

`std::once_flag m_stop_once` 保证 `stop()` 幂等——可安全从信号处理器和 WebUI 多次调用。

---

## 9. 并发模型与线程安全

### 9.1 线程分布

| 线程 | 归属 | 职责 |
|------|------|------|
| io_context 线程 | P2PManager | 所有异步事件、定时器、KCP 更新、消息分发 |
| worker_pool (N线程) | P2PManager | 文件读写 I/O、chunk 压缩/解压 |
| efsw 线程 | StateManager | 文件系统监控通知 |
| hash_pool (N线程) | StateManager | 并行文件 hash 计算 |
| TCP 读写 | TrackerClient | 信令通信 |

### 9.2 锁层级（从外到内）

```
m_transfer_mutex (全局 map 锁, 短暂持有)
    └─ ReceivingFile::file_mutex (per-file 锁, 可长时间持有)

PeerRegistry::m_mutex (shared_mutex, 读多写少)

StateManager::m_change_mutex (保护 pending_changes 队列)

CachedFileStore::m_cache_mutex (shared_mutex)

Database::m_mutex (recursive_mutex, busy_timeout=5000ms)
```

### 9.3 线程安全策略

| 策略 | 应用场景 |
|------|---------|
| post 到 io_context | IceTransport 回调 → 统一在 io_context 线程处理 |
| weak_ptr lambda | 所有跨组件异步回调 |
| atomic 变量 | ReceivingFile::received_chunks, busy; SyncProgress 各字段 |
| thread_local | CryptoLayer 的 EVP_CIPHER_CTX（每线程一个，零竞争） |
| 分层锁 | TransferManager: 全局锁保护结构，per-file 锁保护 I/O |
| shared_mutex | PeerRegistry, CachedFileStore（读多写少场景） |

---

## 10. 设计模式总结

| 模式 | 应用 |
|------|------|
| Factory + enable_shared_from_this | IceTransport、P2PManager、SyncNode — 防止栈分配共享所有权对象 |
| Weak-ptr 回调 | 所有跨组件异步 lambda — 防止 use-after-free |
| 回调注入（DI） | SyncHandler、SyncSession、BroadcastManager、StateManager — 可独立测试 |
| 自适应定时器 | KcpScheduler 使用 `ikcp_check()` — 最小化无效唤醒 |
| Drain/背压 | KCP drain 阈值 → `on_send_ready` 回调 → 节奏发送 |
| Per-file 锁 + 全局结构锁 | TransferManager — 不同文件并行、同文件串行 |
| Thread-local EVP | CryptoLayer — 每线程一个加解密上下文，零竞争 |
| Write-through 缓存 + RYOW | CachedFileStore — 快速读取、事务内一致视图 |
| Anti-entropy 静默定时器 | BroadcastManager 30s — 最终一致性保证 |
| 回声抑制 | StateManager::mark_file_received — 防止循环广播 |
| Bitmap 去重 | TransferManager::received_bitmap — 正确处理 KCP 重传 |
| 幂等关闭 | `std::once_flag` — 信号处理器和 UI 都可安全调用 stop() |
| 两文件配置分离 | config.json (用户) + advanced.json (专家) — 运行时合并 |
| MessageRouter 分发表 | 替代长 if/else 链，注册时声明角色约束 |
