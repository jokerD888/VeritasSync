# VeritasSync 项目深度剖析文档

> 面向初学者的项目架构、组件实现、组件联系与底层原理详解

---

## 目录

1. [项目概述](#1-项目概述)
2. [架构总览](#2-架构总览)
3. [模块详解](#3-模块详解)
   - 3.1 [通用工具层 (common/)](#31-通用工具层-common)
   - 3.2 [网络传输层 (net/)](#32-网络传输层-net)
   - 3.3 [P2P 核心层 (p2p/)](#33-p2p-核心层-p2p)
   - 3.4 [同步逻辑层 (sync/)](#34-同步逻辑层-sync)
   - 3.5 [存储层 (storage/)](#35-存储层-storage)
   - 3.6 [Tracker 服务层 (tracker/)](#36-tracker-服务层-tracker)
   - 3.7 [Web 控制台 (web/)](#37-web-控制台-web)
4. [组件间协作关系](#4-组件间协作关系)
5. [核心协议与算法](#5-核心协议与算法)
6. [线程模型与并发安全](#6-线程模型与并发安全)
7. [生命周期管理](#7-生命周期管理)
8. [构建系统与依赖](#8-构建系统与依赖)
9. [测试体系](#9-测试体系)
10. [阅读路线建议](#10-阅读路线建议)

---

## 1. 项目概述

### 1.1 VeritasSync 是什么

VeritasSync 是一个基于 C++20 的高性能**去中心化 P2P 文件同步工具**。它解决的核心问题是：在复杂网络环境（NAT/防火墙）下，实现多台设备之间安全、高效、自动化的文件双向同步。

### 1.2 核心差异化能力

| 能力 | 技术实现 | 解决的问题 |
|------|----------|------------|
| 可靠 UDP 传输 | KCP 协议 (ARQ) | 弱网环境下 TCP 吞吐量低、延迟高 |
| NAT 穿透 | ICE 协议 (LibJuice) | 不同 NAT 类型设备间无法直接通信 |
| 端到端加密 | AES-256-GCM + HKDF | 防止传输数据被窃听 |
| 断点续传 | Bitmap + 临时文件 | 传输中断后可恢复，无需重传 |
| Tracker 中继回退 | TCP 信令中继 | 对称 NAT 等 ICE 失败场景仍可通信 |
| 冲突自动解决 | 三方合并 + 重命名 | 多端同时修改同一文件不丢数据 |
| AI 忽略规则生成 | LLM + 轻量 RAG | 自然语言生成 .veritasignore 规则 |
| 增量同步 + 回声抑制 | Hash 缓存 + 源头标记 | 只传变化、不传回声 |

### 1.3 两个可执行目标

项目构建产生两个独立的可执行文件：

| 可执行文件 | 入口源文件 | 职责 |
|-----------|-----------|------|
| `veritas_sync` | `src/main.cpp` | 客户端：文件监控 + P2P 同步 + Web 控制台 + 系统托盘 |
| `veritas_tracker` | `src/tracker/main.cpp` | 信令服务器：节点发现 + 信令转发 + 数据中继 |

通过 `CMake -DBUILD_TARGET=CLIENT|TRACKER` 可单独编译其中一个。

---

## 2. 架构总览

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                       用户交互层                                      │
│  ┌────────────────┐   ┌────────────────┐   ┌──────────────────┐    │
│  │  WebUI 控制台   │   │  系统托盘图标   │   │  信号处理 (SIG)   │    │
│  │  (httplib)      │   │  (TrayIcon)     │   │                  │    │
│  └───────┬────────┘   └────────────────┘   └──────────────────┘    │
└──────────┼──────────────────────────────────────────────────────────┘
           │ REST API
┌──────────┼──────────────────────────────────────────────────────────┐
│          ▼       应用编排层 (main.cpp / SyncNode)                    │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │  SyncNode (每个同步任务一个实例)                                │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │   │
│  │  │ P2PManager   │  │TrackerClient │  │  StateManager    │   │   │
│  │  │ (P2P中枢)    │  │ (信令客户端)  │  │  (文件状态管理)   │   │   │
│  │  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘   │   │
│  └─────────┼─────────────────┼───────────────────┼──────────────┘   │
└────────────┼─────────────────┼───────────────────┼──────────────────┘
             │                 │                   │
┌────────────┼─────────────────┼───────────────────┼──────────────────┐
│            ▼       P2P 连接层                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ PeerController (每个远端节点一个)                              │   │
│  │ ┌────────────────────┐  ┌─────────────────────────────┐     │   │
│  │ │   IceTransport     │  │      KcpSession              │     │   │
│  │ │   (NAT穿透/UDP)    │  │      (可靠传输)               │     │   │
│  │ └────────────────────┘  └─────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
             │                                   │
┌────────────┼───────────────────────────────────┼────────────────────┐
│            ▼       同步逻辑层                                      │
│  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────┐   │
│  │  SyncHandler     │  │  SyncSession     │  │  SyncManager    │   │
│  │  (消息处理)      │  │  (会话协商)       │  │  (状态比较算法)  │   │
│  └────────┬─────────┘  └──────────────────┘  └─────────────────┘   │
│           │                                                        │
│  ┌────────▼─────────┐                                             │
│  │  TransferManager │  (分块传输 + 压缩 + 断点续传)                 │
│  └──────────────────┘                                              │
└────────────────────────────────────────────────────────────────────┘
             │
┌────────────┼────────────────────────────────────────────────────────┐
│            ▼       存储与基础层                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐    │
│  │  Database    │  │CachedFileStore│  │    FileFilter          │    │
│  │  (SQLite3)   │  │(Write-Through │  │  (.veritasignore)      │    │
│  │              │  │   缓存层)     │  │                        │    │
│  └──────────────┘  └──────────────┘  └────────────────────────┘    │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐    │
│  │ CryptoLayer  │  │   Hashing    │  │NLFilterGenerator       │    │
│  │ (AES-256-GCM)│  │  (SHA-256)   │  │  (AI忽略规则生成)       │    │
│  └──────────────┘  └──────────────┘  └────────────────────────┘    │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐    │
│  │   Logger     │  │ EncodingUtils│  │     PathUtils          │    │
│  │  (spdlog)    │  │ (UTF-8转换)  │  │  (路径归一化/安全)      │    │
│  └──────────────┘  └──────────────┘  └────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 网络协议栈（数据发送路径）

```
应用层消息
    │
    ▼
CryptoLayer.encrypt()     ← AES-256-GCM 加密
    │
    ▼
BinaryFrame.encode()      ← 封帧 (Magic + Type + Length + Payload)
    │
    ▼
KcpSession.send()         ← 可靠传输 (分片/重传/排序)
    │
    ▼
IceTransport.send()       ← NAT 穿透 (UDP) 或 Tracker 中继
    │
    ▼
互联网
```

数据接收路径为上述过程的逆序。

---

## 3. 模块详解

### 3.1 通用工具层 (common/)

#### Config — 全局配置管理

- **文件**: `include/VeritasSync/common/Config.h`, `src/common/Config.cpp`
- **设计**: 分组嵌套式结构体（`Config::Network`, `Config::Transfer`, `Config::Kcp` 等），将 60+ 配置项按功能域分组
- **双文件分离**: 用户配置存 `config.json`（常改项如 tracker 地址、sync_key），专家调优存 `advanced.json`（如 KCP 窗口大小、拥塞阈值）
- **安全加载**: `LOAD_OPT` 宏实现可选字段安全加载，缺失字段使用默认值
- **校验**: `validate_config()` 检查端口范围、sync_key 合法性等；`is_path_safe()` 防路径穿越攻击

#### CryptoLayer — 端到端加密

- **文件**: `include/VeritasSync/common/CryptoLayer.h`, `src/common/CryptoLayer.cpp`
- **算法**: AES-256-GCM（提供机密性 + 完整性 + 认证）
- **密钥派生**: `set_key()` 使用 HKDF-SHA256 从 sync_key 派生 256 位密钥，应用固定盐值 `"VeritasSync-v1-salt"` 和用途绑定信息 `"veritassync-aes256-gcm"`
- **性能优化**: `thread_local` 缓存 `EVP_CIPHER_CTX`，每个线程独立拥有加解密上下文，彻底消除锁竞争
- **并发安全**: `shared_mutex` 保护密钥读写（写端 `set_key`，读端 `encrypt/decrypt`）
- **安全清理**: 析构时 `OPENSSL_cleanse` 清零密钥内存，防止残留被取证
- **数据格式**: 加密输出 = `IV(12B)` + `Ciphertext` + `Tag(16B)`

#### Hashing — 文件哈希

- **文件**: `include/VeritasSync/common/Hashing.h`, `src/common/Hashing.cpp`
- **三种模式**: `CalculateSHA256`（标准流式读取）、`CalculateSHA256_MMIO`（内存映射，大文件更快）、`CalculateSHA256_Buffer`（内存缓冲区校验）
- **可配置缓冲区**: 通过 `set_read_buffer_size()` 从配置注入，默认 64KB

#### Logger — 异步日志

- **文件**: `include/VeritasSync/common/Logger.h`, `src/common/Logger.cpp`
- **实现**: 基于 spdlog 的异步日志器，全局 `g_logger` 共享指针
- **可配置**: 日志级别、单文件大小、文件数量、线程池大小

#### EncodingUtils — UTF-8 编码转换

- **文件**: `include/VeritasSync/common/EncodingUtils.h`（全部 inline）
- **核心痛点**: Windows 下 `std::filesystem::path` 默认将 `std::string` 视为 ANSI(GBK)，导致中文路径乱码
- **解决方案**: `Utf8ToPath()` 通过 C++20 `char8_t` 桥接 — `std::u8string` 显式告知 path 这是 UTF-8；`PathToUtf8()` 反向转换
- **Win32 API 边界**: `Utf8ToWide()`/`WideToUtf8()` 用于与 Windows API 交互
- **诊断**: `GetLastSystemError()` 同时获取 errno 和 Win32 错误码

#### PathUtils — 路径工具

- **文件**: `include/VeritasSync/common/PathUtils.h`（全部 inline）
- **路径归一化**: 统一分隔符为 `/`，Windows 下转小写，移除尾部斜杠
- **大小写不敏感**: `CaseInsensitiveEqual` 和 `CaseInsensitiveHash` 用于 `unordered_map`，确保 Windows 下路径比较正确
- **路径安全**: `is_path_safe()` 防止路径穿越攻击 — 检查 `..` 组件、绝对路径、规范化后前缀

#### ModernUtils — 现代 C++ 工具集

- **文件**: `include/VeritasSync/common/ModernUtils.h`
- **span 工具**: `as_bytes()`, `make_span()`, `to_string()` — 零拷贝内存视图
- **Error 结构体**: 分类错误（IO/Network/Crypto/Protocol/Timeout/Cancelled），含错误码、消息、上下文
- **Result\<T\> 模板**: 简化版 `std::expected`，基于 `variant<T, Error>`，支持 `map()` 链式调用

#### SignalProto — 信令协议常量

- **文件**: `include/VeritasSync/common/SignalProto.h`
- **内容**: Tracker 信令消息类型常量（REGISTER / REG_ACK / PEER_JOIN / PEER_LEAVE / SIGNAL / RELAY_DATA），由 TrackerClient 和 TrackerServer 共同使用

#### TrayIcon — 系统托盘

- **文件**: `include/VeritasSync/common/TrayIcon.h`, `src/common/TrayIcon.cpp`
- **设计**: PIMPL 模式隔离 Win32 API；原子标志 `m_running` 控制消息循环退出
- **功能**: 菜单项添加、开机自启、退出确认

---

### 3.2 网络传输层 (net/)

#### BinaryFrame — 统一二进制帧协议

- **文件**: `include/VeritasSync/net/BinaryFrame.h`（全部 inline）
- **帧结构**:
  ```
  +--------+----------+-------------+-----------+
  | Magic  | MsgType  | PayloadLen  |  Payload  |
  | 2 bytes| 1 byte   | 4 bytes     |  N bytes  |
  +--------+----------+-------------+-----------+
  ```
  - Magic: `0x56 0x53` ("VS")
  - MsgType: JSON(`0x01`) / BinaryChunk(`0x80`) / BinaryAck(`0x81`)
  - PayloadLen: 网络字节序（大端）
- **安全防护**: `MAX_PAYLOAD_SIZE = 64MB`，编解码两端均校验，防止恶意超大帧导致 OOM
- **辅助函数**: `append_uint16/32`, `read_uint16/32` — 网络字节序读写

#### IceTransport — ICE 传输层

- **文件**: `include/VeritasSync/net/IceTransport.h`, `src/net/IceTransport.cpp`
- **封装对象**: libjuice（C 库，ICE 协议实现）
- **工厂模式**: `create()` 静态方法，两阶段初始化（构造 → `initialize()`），失败返回 nullptr
- **ICE 状态机**: `New → Gathering → Connecting → Connected/Completed/Failed/Disconnected`
- **回调接口**: `IceTransportCallbacks` — 状态变化、本地候选、候选收集完成、数据接收
- **TURN 服务器**: 配置存储为成员变量（非局部变量），因为 `juice_turn_server_t` 保存指向它的指针

#### KcpSession — KCP 可靠传输会话

- **文件**: `include/VeritasSync/net/KcpSession.h`, `src/net/KcpSession.cpp`
- **封装对象**: ikcp（C 库，KCP 协议实现）
- **工厂模式**: `create()` 静态方法
- **核心接口**:
  - `input()`: 喂入底层收到的数据（ICE → KCP）
  - `send()`: 发送应用层消息（KCP 自动分片/重传/排序）
  - `update()`: 驱动 KCP 状态机（需定时调用）
  - `receive()`: 获取完整的应用层消息
- **死锁预防**: `on_output` 回调在 KCP 持锁状态下触发，**禁止**在此回调中调用 KcpSession 的任何方法；`on_message_received` 回调在锁外执行，可安全调用 `send()`
- **KcpContextManager**: 全局单例，管理 KCP 回调上下文生命周期。使用 `weak_ptr<KcpSession>` 避免循环引用，防止回调访问已销毁的 Session
- **自定义删除器**: `KcpDeleter` 在释放 KCP 前先注销上下文

#### UpnpManager — UPnP 端口映射管理

- **文件**: `include/VeritasSync/net/UpnpManager.h`, `src/net/UpnpManager.cpp`
- **用途**: 在支持 UPnP 的路由器上自动添加端口映射，将 host 候选地址替换为公网 IP
- **异步初始化**: `init_async()` 在线程池中执行 UPnP 发现（耗时 2-3 秒）
- **候选重写**: `rewrite_candidate()` 将 host 类型候选的局域网 IP 替换为公网 IP

---

### 3.3 P2P 核心层 (p2p/)

#### P2PManager — P2P 连接中枢

- **文件**: `include/VeritasSync/p2p/P2PManager.h`, `src/p2p/P2PManager.cpp`
- **核心职责**: 管理所有 PeerController、消息加密/解密/路由、同步会话协调
- **独立事件循环**: 持有 `io_context` + `jthread`，所有 P2P 网络事件在此线程处理
- **Peer 管理**: `m_peers` 映射（`peer_id → shared_ptr<PeerController>`），使用 `shared_mutex`（读并发、写互斥）
- **子组件**: 持有 `TransferManager`、`SyncHandler`、`SyncSession`、`KcpScheduler`、`UpnpManager`
- **消息流程**: 
  1. 收到 KCP 消息 → `handle_kcp_message()` 解密 → `dispatch_json_message()` 路由
  2. 路由到 `SyncHandler`（文件操作消息）或 `SyncSession`（会话握手消息）
- **Anti-Entropy 对账**: 30 秒静默后自动触发 `trigger_reconciliation()` 全量 flood sync
- **中继回退**: ICE 失败时 `attempt_relay_fallback()` 通过 Tracker TCP 中继数据
- **广播方法**: `broadcast_file_update()`、`broadcast_file_updates_batch()` 等带 KCP 流控背压
- **优雅关闭**: `shutdown_gracefully()` 广播 goodbye 消息并等待 KCP 发送完成

#### PeerController — 单节点连接控制器

- **文件**: `include/VeritasSync/p2p/PeerController.h`, `src/p2p/PeerController.cpp`
- **组合关系**: 组合 `IceTransport` + `KcpSession`，管理单个 Peer 的连接生命周期
- **角色判定**: `self_id < peer_id` → Offer 方（控制方）；反之 → Answer 方（受控方）
- **两阶段初始化**: 
  1. `initialize_ice()`: 创建 IceTransport
  2. `bind_callbacks()`: 在 shared_ptr 就绪后绑定回调（使用 `weak_ptr` 防悬垂）
- **连接流程**: `initiate_connection()` → ICE Gathering → SDP Offer/Answer 交换 → ICE Connected → `setup_kcp_session()` 创建 KCP
- **中继模式**: `enable_relay_mode()` 切换到 Tracker 中继通道；`feed_relay_data()` 喂入中继数据
- **同步会话状态**: 原子变量管理 session_id、expected_file_count、received_file_count 等
- **超时管理**: `start_sync_timeout()` / `refresh_sync_timeout()` 线程安全地管理同步超时定时器

#### TrackerClient — 信令服务器客户端

- **文件**: `include/VeritasSync/p2p/TrackerClient.h`, `src/p2p/TrackerClient.cpp`
- **传输协议**: TCP 长连接 + 4 字节长度前缀 + JSON 消息
- **生命周期**: `connect()` → `REGISTER` → 收到 `REG_ACK` + `PEER_JOIN` → 回调通知上层
- **自动重连**: 5 秒间隔自动重连，写队列保证消息不丢失
- **消息处理器映射**: `m_handlers` 将消息类型映射到处理函数
- **中继数据通道**: `send_relay_data()` / `set_relay_callback()` 支持通过 Tracker 中继二进制数据

#### KcpScheduler — KCP 自适应更新调度器

- **文件**: `include/VeritasSync/p2p/KcpScheduler.h`, `src/p2p/KcpScheduler.cpp`
- **自适应频率**: 根据活跃度动态调整 KCP update 间隔
  - 活跃期: 5ms（有待发送数据）
  - 近期活跃: 10ms（5 秒内有过活跃）
  - 空闲期: 100ms（超过 5 秒无活跃）
- **心跳保活**: 每 15 秒发送 PING，保持 NAT 映射存活

#### WebUIServer — Web 控制台

- **文件**: `include/VeritasSync/p2p/WebUI.h`, `src/p2p/WebUI.cpp`
- **实现**: 基于 httplib 的 HTTP 服务器
- **安全防护**: Token 认证、CSP 安全头、XSS 防护（动态内容全量转义）、敏感字段脱敏
- **路由拆分**: 页面路由、状态路由、配置路由、任务路由、忽略规则路由、NL 过滤路由
- **AI 集成**: 内嵌 `NLFilterGenerator` 实例，支持自然语言生成忽略规则

---

### 3.4 同步逻辑层 (sync/)

#### SyncNode — 同步任务核心协调器

- **文件**: `include/VeritasSync/sync/SyncNode.h`, `src/sync/SyncNode.cpp`
- **设计**: `enable_shared_from_this` + 私有构造函数，强制通过 `shared_ptr` 创建，防止异步回调 Use-After-Free
- **原子智能指针**: `atomic<shared_ptr<TrackerClient>>` 和 `atomic<shared_ptr<P2PManager>>`，线程安全地管理核心组件
- **启动流程** (`start()`):
  1. 配置验证（sync_key、sync_folder、role）
  2. 创建 P2PManager（注入性能参数）
  3. 创建 TrackerClient（共享 P2PManager 的 io_context）
  4. 互相注入依赖
  5. 配置加密密钥（`"encrypt:" + sync_key`，与 Tracker 房间键分离）
  6. 创建 StateManager（注入回调连接到 P2PManager 广播方法）
  7. 初始扫描
  8. 连接 Tracker（使用 `weak_ptr` 保证回调生命周期安全）
- **停止流程** (`stop()`):
  1. `call_once` 保证只执行一次
  2. P2PManager 优雅关闭（广播 goodbye）
  3. 断开 TrackerClient → P2PManager 反向引用
  4. 断开 P2PManager → StateManager 裸指针
  5. 断开 P2PManager → TrackerClient 裸指针
  6. 销毁 StateManager、P2PManager、TrackerClient

#### SyncManager — 纯算法：状态比较

- **文件**: `include/VeritasSync/sync/SyncManager.h`, `src/sync/SyncManager.cpp`
- **核心方法**: `compare_states_and_get_requests()` — 比较本地和远程文件列表，返回操作清单
- **三方合并冲突检测**: `detect_conflict()` — 比较 local_hash / remote_hash / base_hash（历史记录），判断是否冲突
- **操作清单**: `SyncActions`（files_to_request / files_to_delete / files_to_conflict_rename）+ `DirSyncActions`（dirs_to_create / dirs_to_delete）
- **执行顺序约定**: 创建目录 → 冲突重命名 → 下载文件 → 删除文件 → 删除目录

#### SyncHandler — 同步消息处理器

- **文件**: `include/VeritasSync/sync/SyncHandler.h`, `src/sync/SyncHandler.cpp`
- **设计**: 从 P2PManager 提取的纯业务逻辑层，不持有网络资源，通过回调与 P2PManager 交互
- **处理的消息**: `share_state`、`file_update`、`file_delete`、`dir_create`、`dir_delete` 及其批量版本
- **冲突解决**: `resolve_conflict()` — 当双方都修改时，自动将本地文件重命名为 `filename.conflict.{timestamp}.ext`，然后下载远程版本
- **断点续传**: `build_file_request()` 检查本地是否有未完成的传输，有则携带 `start_chunk` 请求续传

#### SyncSession — 同步会话管理

- **文件**: `include/VeritasSync/sync/SyncSession.h`, `src/sync/SyncSession.cpp`
- **设计**: 从 P2PManager 提取的同步会话协调逻辑
- **功能**:
  - `perform_flood_sync()`: 全量推送文件/目录状态给对端
  - `send_sync_begin()` / `handle_sync_begin()`: 同步会话开始握手
  - `send_sync_ack()` / `handle_sync_ack()`: 同步会话确认（含重试逻辑）
- **流控**: 当 KCP 发送队列超过阈值时自动等待

#### TransferManager — 分块传输管理

- **文件**: `include/VeritasSync/sync/TransferManager.h`, `src/sync/TransferManager.cpp`
- **上传路径** (`queue_upload()`): 读取文件 → 分块 → Snappy 压缩 → 加密 → 通过回调发送
- **下载路径** (`handle_chunk()`): 解密（P2PManager 完成）→ 解压 → 解析头部 → 写入临时文件 → 全部收齐后重命名
- **断点续传**: 
  - `check_resume_eligibility()`: 检查内存中是否有未完成传输 + 校验源文件未变
  - `register_expected_metadata()`: 预注册接收任务的元数据
  - Bitmap 追踪: `received_bitmap` 标记每个 chunk 是否已接收，正确处理 KCP 重传导致的重复
- **并发安全**: 
  - 全局锁 `m_transfer_mutex` 保护 map 结构
  - per-file 锁 `ReceivingFile::file_mutex` 允许不同文件并行处理
  - `atomic<bool> busy` 防止 cancel/cleanup 与 handle_chunk 并发
- **流控**: 拥塞检测 — KCP 发送队列超过阈值时等待；速度计算和停滞检测

#### Protocol — 协议消息类型定义

- **文件**: `include/VeritasSync/sync/Protocol.h`
- **核心结构**: `FileInfo`（path / hash / mtime / size）
- **消息类型常量**: share_state, request_file, file_chunk, file_update, file_delete, dir_create, dir_delete, file_update_batch, file_delete_batch, dir_batch, sync_begin, sync_ack, sync_complete, goodbye

---

### 3.5 存储层 (storage/)

#### Database — SQLite3 持久化

- **文件**: `include/VeritasSync/storage/Database.h`, `src/storage/Database.cpp`
- **模式**: WAL 模式，支持并发读写
- **表结构**: `files`（path, hash, mtime）+ `sync_history`（peer_id, path, hash, ts）
- **预编译语句**: 所有常用 SQL 预编译为 `ScopedStmt`，使用 RAII 自动管理生命周期
- **事务**: `TransactionGuard` RAII Guard — 构造时加锁+开启事务，析构时若未 commit 则 rollback
- **递归锁**: `recursive_mutex` 允许同一线程多次加锁（事务嵌套场景）

#### CachedFileStore — Write-Through 缓存层

- **文件**: `include/VeritasSync/storage/CachedFileStore.h`, `src/storage/CachedFileStore.cpp`
- **设计**: 封装 Database 的文件元数据操作，所有写操作先持久化到 DB，成功后自动更新内存缓存
- **事务感知**: `CacheAwareGuard` — 事务期间缓存更新暂存于 `m_pending_ops`，commit 时应用，rollback 时丢弃
- **启动加载**: `load_cache()` 批量加载 DB 中所有文件元数据到内存
- **读操作**: `get()` / `get_all_paths()` — cache-first，`shared_lock` 并发读

#### StateManager — 文件状态管理

- **文件**: `include/VeritasSync/storage/StateManager.h`, `src/storage/StateManager.cpp`
- **核心职责**: 管理本地文件系统状态（扫描、监控、记录修改）
- **文件监控**: 基于 efsw 的文件系统监控，`UpdateListener` 回调通知变化
- **防抖机制**: 文件变化后等待 5 秒（可配置）再处理，避免频繁触发
- **阈值触发**: 变更积累超过阈值时立即触发批处理，不等待防抖
- **三阶段处理**:
  1. Phase 1 (独立线程): 遍历变更文件，计算 hash，检测回声，分类为文件更新/删除/目录变更
  2. Phase 2 (io_context): 更新 m_file_map 和数据库
  3. Phase 3 (io_context): 通过回调通知上层（P2PManager 广播变更）
- **回声抑制**: `mark_file_received()` 记录从对端接收的文件；`check_and_clear_echo()` 在广播前检查并跳过回声
- **同步历史**: `should_ignore_echo()` / `record_file_sent()` / `record_sync_success()` 管理同步历史，支持三方合并

#### FileFilter — 文件过滤

- **文件**: `include/VeritasSync/storage/FileFilter.h`, `src/storage/FileFilter.cpp`
- **规则来源**: 内置默认规则 + 用户 `.veritasignore` 文件
- **语法**: 兼容 `.gitignore`，支持 `*`/`**`/`!`/`[a-z]` 等
- **双层匹配**: 快速路径（后缀/目录名直接匹配）+ 慢速路径（正则匹配）
- **C++20 异构查找**: `StringHash` + `std::equal_to<>` 支持 `string_view` 查找，零拷贝

#### NLFilterGenerator — AI 忽略规则生成

- **文件**: `include/VeritasSync/storage/NLFilterGenerator.h`, `src/storage/NLFilterGenerator.cpp`
- **架构**: LLM + 轻量 RAG，全部 C++ 实现
- **RAG 两层**:
  1. 目录摘要: `build_directory_summary()` 压缩文件树为按目录聚合的统计信息（5 万文件 → <3KB）
  2. 关键词采样: `search_relevant_samples()` 从文件树搜索匹配样本 + 同级对比文件
- **LLM 调用**: OpenAI 兼容格式，支持阿里百炼/DeepSeek/OpenAI
- **三层解析防线**: API 级 `json_object` 约束 → Prompt 死命令 → C++ 大括号提取法 + 逐行 Fallback

---

### 3.6 Tracker 服务层 (tracker/)

#### TrackerServer — 信令服务器

- **文件**: `include/VeritasSync/tracker/TrackerServer.h`, `src/tracker/TrackerServer.cpp`
- **职责**: 按 sync_key 分组管理 Session，转发信令消息，中继数据
- **数据结构**: `m_peer_groups`（sync_key → Session 集合）+ `m_peers_by_id`（peer_id → Session）

#### Session — TCP 会话

- **文件**: `include/VeritasSync/tracker/Session.h`, `src/tracker/Session.cpp`
- **协议**: 4 字节长度前缀 + JSON 消息
- **消息处理**: REGISTER → 加入房间 + 广播 PEER_JOIN；SIGNAL → 转发给目标；RELAY_DATA → 中继二进制数据

---

### 3.7 Web 控制台 (web/)

- **文件**: `src/web/index.html`（82KB，赛博朋克风格单页应用）
- **功能**: 任务管理、实时状态监控、传输进度、对等点详情、忽略规则编辑、AI 规则生成

---

## 4. 组件间协作关系

### 4.1 依赖注入图

```
main.cpp (应用编排)
  │
  ├─→ SyncNode::create(task, config)
  │     │
  │     ├─→ P2PManager::create(perf_config)
  │     │     │
  │     │     ├─→ TransferManager(sm, io_context, pool, send_cb, chunk_size)
  │     │     ├─→ SyncHandler(sm, transfer_mgr, pool, io_context, send_cb, ...)
  │     │     ├─→ SyncSession(sm, pool, io_context, send_cb, ...)
  │     │     ├─→ KcpScheduler(io_context, collect_peers_cb)
  │     │     └─→ UpnpManager (内置)
  │     │
  │     ├─→ TrackerClient(io_context, host, port)
  │     │     └─→ set_p2p_manager(p2p)     // 注入反向引用
  │     │
  │     ├─→ StateManager(root, io_context, callbacks, enable_watcher)
  │     │     ├─→ Database(db_path)
  │     │     ├─→ CachedFileStore(db)
  │     │     └─→ FileFilter (内置)
  │     │
  │     ├─→ p2p->set_tracker_client(tracker)  // 注入
  │     ├─→ p2p->set_state_manager(sm)        // 注入
  │     └─→ tracker->connect(sync_key, on_ready_cb)
  │
  └─→ WebUIServer(port, config_path)
        └─→ NLFilterGenerator (内置)
```

### 4.2 关键回调连接

| 回调 | 来源 | 目标 | 用途 |
|------|------|------|------|
| `sm_callbacks.on_file_updates` | StateManager | P2PManager | 文件变化时广播 |
| `sm_callbacks.on_file_deletes` | StateManager | P2PManager | 文件删除时广播 |
| `sm_callbacks.on_dir_changes` | StateManager | P2PManager | 目录变化时广播 |
| `PeerController::on_signal_needed` | PeerController | TrackerClient | ICE 信令转发 |
| `PeerController::on_message_received` | PeerController | P2PManager | 消息路由 |
| `TrackerClient::m_relay_callback` | TrackerClient | P2PManager | 中继数据接收 |
| `TransferManager::m_send_callback` | TransferManager | P2PManager | 加密数据发送 |

### 4.3 消息处理流程

```
KCP 收到原始数据
  │
  ▼
PeerController.on_kcp_message_received(encrypted_msg)
  │
  ▼
P2PManager.handle_kcp_message(msg, from_peer)
  │ ── CryptoLayer.decrypt() ──
  │ ── JSON 解析 ──
  │
  ▼
P2PManager.dispatch_json_message(msg_type, payload, from_peer, can_receive)
  │
  ├── share_state → SyncHandler.handle_share_state()
  ├── file_update → SyncHandler.handle_file_update()
  ├── file_delete → SyncHandler.handle_file_delete()
  ├── dir_create  → SyncHandler.handle_dir_create()
  ├── dir_delete  → SyncHandler.handle_dir_delete()
  ├── *_batch     → SyncHandler.handle_*_batch()
  ├── sync_begin  → SyncSession.handle_sync_begin()
  ├── sync_ack    → SyncSession.handle_sync_ack()
  ├── request_file → TransferManager.queue_upload()
  └── goodbye     → P2PManager.handle_goodbye()
```

---

## 5. 核心协议与算法

### 5.1 ICE 连接建立流程

```
节点 A (Offer方, self_id < peer_id)    Tracker Server    节点 B (Answer方)
    │                                       │                   │
    │  ── REGISTER ─────────────────────→   │                   │
    │  ←── REG_ACK + PEER_JOIN(B) ───────   │                   │
    │                                       │ ←── REGISTER ──── │
    │                                       │ ─── PEER_JOIN(A)→ │
    │                                       │                   │
    │  PeerController.initiate_connection() │                   │
    │  ↓ ICE Gathering (收集本地候选)        │                   │
    │                                       │                   │
    │  ── SIGNAL(ice_offer, SDP) ────────→  │ ── SIGNAL ──────→ │
    │                                       │                   │
    │                                       │  B.set_remote_description(offer)
    │                                       │  B.gather_candidates()
    │                                       │                   │
    │  ←── SIGNAL(ice_answer, SDP) ────── ←─ ── SIGNAL ─────── │
    │                                       │                   │
    │  A.set_remote_description(answer)     │                   │
    │                                       │                   │
    │  ←────── ice_candidate (多次交换) ──────────────────────→ │
    │                                       │                   │
    │  ←════════════ ICE Connected ══════════════════════════→ │
    │  ↓ setup_kcp_session()               │  ↓ setup_kcp_session()
    │  ←════════════ KCP 加密通信 ═══════════════════════════→ │
```

### 5.2 同步握手协议 (sync_begin / sync_ack)

```
Source                                          Destination
  │                                                  │
  │── sync_begin {session_id, file_count, dir_count}→│
  │                                                  │ 设置预期数量
  │←── sync_ack {session_id, received_files,         │
  │              received_dirs, missing_files,        │
  │              missing_dirs} ──────────────────────│
  │                                                  │
  │── 补发缺失的文件/目录信息 ───────────────────────→│
  │                                                  │
  │── sync_complete {session_id} ───────────────────→│
```

### 5.3 文件传输协议 (request_file / file_chunk)

```
Destination                                     Source
  │                                                │
  │── request_file {path, hash, size,              │
  │                 start_chunk(断点续传)} ────────→│
  │                                                │ TransferManager.queue_upload()
  │                                                │ ↓ 读取文件 → 分块 → Snappy 压缩
  │                                                │
  │←── BinaryFrame(BINARY_CHUNK) ─────────────────│ chunk_1
  │←── BinaryFrame(BINARY_CHUNK) ─────────────────│ chunk_2
  │←── ...                                        │
  │←── BinaryFrame(BINARY_CHUNK) ─────────────────│ chunk_N
  │                                                │
  │  TransferManager.handle_chunk()                │
  │  ↓ 解压 → 写入临时文件 → Bitmap 标记           │
  │  ↓ 全部收齐 → Hash 校验 → 重命名为目标文件      │
```

### 5.4 三方合并冲突检测

```
                 base_hash (上次同步时的哈希)
                     │
         ┌───────────┼───────────┐
         │                       │
    local_hash              remote_hash
         │                       │
         ▼                       ▼
  本地修改了?                  远程修改了?
  
  场景1: local==base, remote!=base → 下载远程 (RequestRemote)
  场景2: local!=base, remote==base → 保留本地 (Skip)
  场景3: local==remote             → 已一致 (NoAction)
  场景4: local!=base, remote!=base → 冲突!
    → 重命名本地为 .conflict.{timestamp}.ext
    → 下载远程版本 (RequestRemote)
  场景5: 无历史记录 (base=nullopt)
    → local!=remote → 冲突 (同场景4处理)
```

### 5.5 回声抑制算法

```
节点 A 下载了文件 F (hash=H1)
  │
  ▼
StateManager.mark_file_received("F", "H1")
  → m_received_files["F"] = "H1"
  │
  ... 一段时间后 ...
  │
efsw 检测到文件 F 变化 (可能是写入完成事件)
  │
  ▼
StateManager 检查: check_and_clear_echo("F", "H1")
  → m_received_files["F"] == "H1" → 匹配!
  → 删除记录, 返回 true → 跳过广播
```

---

## 6. 线程模型与并发安全

### 6.1 线程分布

| 线程 | 所属模块 | 职责 |
|------|----------|------|
| P2PManager IO 线程 | P2PManager (`m_thread`) | io_context 事件循环，处理所有网络回调 |
| Worker 线程池 | P2PManager (`m_worker_pool`) | Hash 计算、文件 IO、压缩加密等耗时操作 |
| WebUI 线程 | main.cpp (`ui_thread`) | httplib 服务器阻塞循环 |
| Main 线程 | main.cpp | 托盘消息循环 / 等待关闭信号 |
| libjuice 内部线程 | IceTransport | ICE 候选收集、连接检查 |
| StateManager Phase 1 线程 | StateManager | Hash 计算等 CPU 密集操作 |

### 6.2 线程安全机制一览

| 机制 | 使用位置 | 保护对象 |
|------|----------|----------|
| `shared_mutex` | P2PManager::m_peers_mutex | PeerController 映射（读并发/写互斥） |
| `shared_mutex` | CryptoLayer::m_key_mutex | 密钥读写 |
| `shared_mutex` | CachedFileStore::m_mutex | 缓存读写 |
| `shared_mutex` | FileFilter::m_mutex | 规则读写 |
| `mutex` | PeerController::m_mutex | ICE/KCP 资源、定时器 |
| `mutex` | TransferManager::m_transfer_mutex | 接收/发送文件映射 |
| `recursive_mutex` | Database::m_mutex | 数据库操作 |
| `mutex` | StateManager::m_file_map_mutex | 文件状态映射 |
| `mutex` | StateManager::m_changes_mutex | 待处理变更集合 |
| `atomic<>` | 多处 | 状态标志、计数器、智能指针 |
| `thread_local` | CryptoLayer | EVP_CIPHER_CTX 加解密上下文 |
| `call_once` | SyncNode::m_stop_once | 停止操作只执行一次 |
| `weak_ptr + post` | PeerController 回调 | 防止悬垂指针（ICE 回调中对象可能已销毁） |
| per-file mutex | TransferManager::ReceivingFile::file_mutex | 同一文件串行写入，不同文件并行 |

---

## 7. 生命周期管理

### 7.1 SyncNode 启动时序

```
1. 配置验证 (sync_key / sync_folder / role)
2. 创建 P2PManager (性能参数注入)
3. 创建 TrackerClient (共享 P2PManager 的 io_context)
4. 互相注入: tracker↔p2p
5. 配置 P2PManager (role / encryption_key / mode / STUN / TURN)
6. 创建 StateManager (回调连接 P2PManager 广播)
7. 注入 StateManager → P2PManager
8. 初始扫描: sm.scan_directory()
9. 连接 Tracker: tracker.connect(sync_key, on_ready)
   └── on_ready 回调: p2p.connect_to_peers(peer_list)
```

### 7.2 SyncNode 停止时序（关键：防止 Use-After-Free）

```
1. call_once 保证只执行一次
2. P2PManager.shutdown_gracefully()  ← 广播 goodbye, 等 KCP 发完
3. TrackerClient.stop()             ← 关闭 TCP 连接
4. TrackerClient.set_p2p_manager(nullptr)  ← 断开反向引用
5. P2PManager.set_state_manager(nullptr)    ← 断开对 StateManager 的引用
6. P2PManager.set_tracker_client(nullptr)   ← 断开对 TrackerClient 的引用
7. StateManager.reset()             ← 销毁 StateManager
8. P2PManager.store(nullptr)        ← 销毁 P2PManager
9. TrackerClient.store(nullptr)     ← 销毁 TrackerClient
```

**顺序为什么重要**: P2PManager 的 worker_pool / io_context 线程可能还在执行异步任务，这些任务通过裸指针访问 StateManager/TrackerClient。必须在销毁这些对象前先将指针置空，使 null 检查能安全短路。

---

## 8. 构建系统与依赖

### 8.1 构建配置

- **构建系统**: CMake 3.15+ + Ninja
- **C++ 标准**: C++20（`jthread`, `span`, `shared_mutex`, `atomic<shared_ptr>`, `char8_t`）
- **CMake Presets**: `debug` (Ninja + Debug) / `release` (Ninja + Release)
- **分离编译**: `-DBUILD_TARGET=CLIENT|TRACKER` 可只编译客户端或 Tracker
- **Release 优化**: MSVC `/O2 /GL` + `/LTCG`；GCC/Clang `-O3 -march=native`

### 8.2 第三方依赖 (vcpkg Manifest Mode)

| 依赖 | 版本 | 用途 |
|------|------|------|
| Boost (Asio/System/UUID/Interprocess) | 1.88+ | 异步 IO、UUID 生成 |
| OpenSSL | - | AES-256-GCM、HKDF-SHA256 |
| KCP | - | 可靠 UDP 传输 |
| LibJuice | - | ICE 协议 (STUN/TURN) |
| miniupnpc | - | UPnP 端口映射 |
| nlohmann/json | - | JSON 序列化 |
| Snappy | - | 数据压缩 |
| spdlog | - | 异步日志 |
| cpp-httplib | - | HTTP 服务器 (WebUI) |
| SQLite3 | - | 文件元数据持久化 |
| efsw | - | 文件系统监控 |
| b64 | - | Base64 编解码 |
| GTest | - | 单元测试框架 |

---

## 9. 测试体系

### 9.1 测试规模

- **32 个测试文件**，**557 个测试用例**
- 统一入口: `unit_tests` 可执行文件
- 测试基础设施: `tests/test_helpers.h`（全局 Logger 初始化、`make_file()` 辅助、`cleanup_db_files()` 辅助）

### 9.2 测试文件与覆盖模块

| 测试文件 | 覆盖模块 |
|----------|----------|
| test_sync_logic.cpp | SyncManager (状态比较算法) |
| test_protocol.cpp | Protocol (消息类型) |
| test_hashing.cpp | Hashing (SHA-256) |
| test_database.cpp / test_database_advanced.cpp / test_database_coverage.cpp | Database (SQLite CRUD) |
| test_binary_frame.cpp / test_binary_frame_coverage.cpp | BinaryFrame (帧编解码) |
| test_crypto_layer.cpp | CryptoLayer (AES-256-GCM) |
| test_encoding_utils.cpp | EncodingUtils (UTF-8 转换) |
| test_file_filter.cpp | FileFilter (.veritasignore 规则) |
| test_kcp_session.cpp | KcpSession (可靠传输) |
| test_peer_controller.cpp | PeerController (单节点连接) |
| test_p2p_manager.cpp | P2PManager (P2P 中枢) |
| test_nl_filter.cpp | NLFilterGenerator (AI 规则生成) |
| test_sync_handler.cpp | SyncHandler (消息处理) |
| test_sync_session.cpp | SyncSession (会话管理) |
| test_sync_node.cpp | SyncNode (任务协调) |
| test_transfer_manager.cpp / test_transfer_manager_async.cpp / test_transfer_manager_coverage.cpp | TransferManager (文件传输) |
| test_config.cpp | Config (配置加载/验证) |
| test_parallel_scan.cpp | StateManager (并行扫描) |
| test_modern_utils.cpp | ModernUtils (span/Result) |
| test_common_utils_robustness.cpp | 通用工具健壮性 |
| test_state_manager_enhanced.cpp / test_state_manager_coverage.cpp | StateManager (文件状态管理) |
| test_logger_coverage.cpp | Logger |

---

## 10. 阅读路线建议

### 路线一：自底向上（推荐初学者）

理解底层组件后，上层组件自然容易理解：

1. **common/** → Config, Logger, Hashing, EncodingUtils, PathUtils, ModernUtils
2. **storage/** → Database, CachedFileStore, FileFilter
3. **common/** → CryptoLayer (理解加密后再看网络层)
4. **net/** → BinaryFrame, IceTransport, KcpSession, UpnpManager
5. **sync/** → Protocol, SyncManager (纯算法), SyncHandler, SyncSession, TransferManager
6. **p2p/** → TrackerClient, PeerController, KcpScheduler, P2PManager
7. **sync/** → SyncNode (理解所有组件后看编排)
8. **tracker/** → TrackerServer, Session
9. **main.cpp** → 理解整个应用的启动/关闭流程

### 路线二：自顶向下（适合有经验的开发者）

先理解全局，再深入细节：

1. **main.cpp** → 理解应用启动、配置加载、SyncNode 生命周期
2. **SyncNode** → 理解组件编排和依赖注入
3. **P2PManager** → 理解消息路由和同步协调
4. **PeerController** → 理解 ICE+KCP 的组合
5. **SyncHandler + TransferManager** → 理解同步逻辑和文件传输
6. **IceTransport + KcpSession** → 理解底层网络传输

### 关键源文件大小参考（按行数排序，帮助评估阅读量）

| 文件 | 行数 | 核心度 |
|------|------|--------|
| P2PManager.cpp | ~1200 | ★★★★★ |
| TransferManager.cpp | ~981 | ★★★★★ |
| StateManager.cpp | ~991 | ★★★★☆ |
| SyncHandler.cpp | ~900 | ★★★★☆ |
| WebUI.cpp | ~1009 | ★★★☆☆ |
| PeerController.cpp | ~600 | ★★★★★ |
| IceTransport.cpp | ~500 | ★★★★☆ |
| TrackerClient.cpp | ~500 | ★★★☆☆ |
| NLFilterGenerator.cpp | ~700 | ★★☆☆☆ |
| Config.h | ~485 | ★★★☆☆ |
| SyncNode.cpp | ~334 | ★★★★★ |

---

> 本文档最后更新：2026-04-24
