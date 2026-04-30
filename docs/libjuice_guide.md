# libjuice 深度解析：从原理到实践

> 基于官方仓库 [paullouisageneau/libjuice](https://github.com/paullouisageneau/libjuice) 撰写  
> 作者：Paul-Louis Ageneau | 开源协议：MPL 2.0 | 版本：1.7.1

---

## 目录

1. [概述](#1-概述)
2. [背景知识：NAT 与 ICE](#2-背景知识nat-与-ice)
3. [核心原理](#3-核心原理)
   - 3.1 [ICE 协议工作流程](#31-ice-协议工作流程)
   - 3.2 [STUN 协议](#32-stun-协议)
   - 3.3 [TURN 中继](#33-turn-中继)
   - 3.4 [SDP 交互](#34-sdp-交互)
4. [协议报文格式](#4-协议报文格式)
5. [架构设计](#5-架构设计)
   - 5.1 [整体架构](#51-整体架构)
   - 5.2 [模块划分](#52-模块划分)
   - 5.3 [并发模型](#53-并发模型)
   - 5.4 [核心数据结构](#54-核心数据结构)
6. [实现详解](#6-实现详解)
   - 6.1 [ICE Agent 生命周期](#61-ice-agent-生命周期)
   - 6.2 [候选者收集](#62-候选者收集)
   - 6.3 [连接性检查](#63-连接性检查)
   - 6.4 [候选者对选择](#64-候选者对选择)
   - 6.5 [STUN 消息处理](#65-stun-消息处理)
   - 6.6 [TURN 中继实现](#66-turn-中继实现)
   - 6.7 [ICE-TCP 支持](#67-ice-tcp-支持)
   - 6.8 [Consent Freshness](#68-consent-freshness)
   - 6.9 [PAC 定时器](#69-pac-定时器)
   - 6.10 [STUN 服务器端实现](#610-stun-服务器端实现)
7. [特性与限制](#7-特性与限制)
8. [API 参考](#8-api-参考)
9. [使用示例](#9-使用示例)
10. [编译与集成](#10-编译与集成)
11. [与 libnice 的详细比较](#11-与-libnice-的详细比较)
12. [最佳实践](#12-最佳实践)
13. [生态与衍生项目](#13-生态与衍生项目)
14. [常见问题](#14-常见问题)
15. [附录：源码常量定义](#15-附录源码常量定义)

---

## 1. 概述

libjuice（**J**UICE is a **U**DP **I**nteractive **C**onnectivity **E**stablishment library）是一个轻量级的 ICE 协议实现库，用于在 NAT（网络地址转换）环境下建立双向 UDP 通信通道。

### 核心特征

| 特性    | 说明                                             |
| ----- | ---------------------------------------------- |
| 语言    | 纯 C（C11 标准）                                    |
| 依赖    | **零依赖**（可选 Nettle 提供加密哈希）                      |
| 协议标准  | RFC 8445 (ICE)、RFC 8489 (STUN)、RFC 8656 (TURN) |
| 许可证   | MPL 2.0                                        |
| 平台    | Linux、Android、FreeBSD、macOS、iOS、Windows        |
| 代码规模  | ~5000 行 C 代码（21 个 .c 文件）                       |
| 公共头文件 | 仅 `juice/juice.h` 一个                           |

### 设计哲学

libjuice 是 ICE 协议的**简化实现**，专注于最常见场景：

> "The client supports only a single component over UDP per session in a standard single-gateway network topology, as this should be sufficient for the majority of use cases nowadays."

这种"够用即可"的设计使得 libjuice 极其轻量且易于集成，同时覆盖了绝大多数实际需求（如 WebRTC Data Channel、RTP/RTCP 复用流等）。

---

## 2. 背景知识：NAT 与 ICE

### 2.1 NAT 问题

在互联网中，大部分设备位于 NAT（Network Address Translator）之后。NAT 将内部私有地址映射为外部公网地址，导致：

- 内部设备可以主动发往外网，但外网无法直接访问内部设备
- 两个不同 NAT 后的设备无法直接通信

NAT 类型从严格到宽松分为：

| NAT 类型               | 描述                  | 穿透难度 |
| -------------------- | ------------------- | ---- |
| Full Cone            | 映射后任何外部地址都可访问       | 容易   |
| Restricted Cone      | 仅允许曾发过包的 IP 访问      | 中等   |
| Port Restricted Cone | 仅允许曾发过包的 IP:Port 访问 | 较难   |
| Symmetric            | 每个目标使用不同映射端口        | 困难   |

### 2.2 ICE 解决方案

ICE（Interactive Connectivity Establishment，交互式连接建立）是一个系统化的 NAT 穿透框架：

1. **收集候选地址**（Candidate Gathering）：获取所有可能的通信地址
2. **交换候选地址**：通过信令服务器交换双方的候选地址
3. **连接性检查**（Connectivity Check）：逐对测试候选地址对的可达性
4. **选择最优路径**：选择优先级最高的可用地址对

### 2.3 候选地址类型

| 类型      | 英文               | 来源         | 优先级      |
| ------- | ---------------- | ---------- | -------- |
| 主机候选    | Host             | 本地网络接口     | 最高 (126) |
| 对端反射候选  | Peer Reflexive   | 连接性检查中发现   | 高 (110)  |
| 服务器反射候选 | Server Reflexive | STUN 服务器发现 | 中 (100)  |
| 中继候选    | Relayed          | TURN 服务器分配 | 最低 (0)   |

---

## 3. 核心原理

### 3.1 ICE 协议工作流程

```
     Agent A                          信令服务器                        Agent B
       │                                 │                               │
       │  1. 创建 Agent                  │                               │
       │  2. 收集本地候选地址              │                               │
       │  3. 向 STUN 服务器获取反射地址    │                               │
       │  4. (可选) 向 TURN 服务器申请中继 │                               │
       │                                 │                               │
       │──── SDP Offer (含本地描述) ──────►│──── 转发 SDP Offer ──────────►│
       │                                 │                               │
       │◄──── SDP Answer (含远端描述) ────│◄─── 转发 SDP Answer ──────────│
       │                                 │                               │
       │  5. 交换 Trickle 候选者          │  5. 交换 Trickle 候选者        │
       │──── candidate:... ──────────────►│──── candidate:... ──────────►│
       │◄──── candidate:... ─────────────│◄─── candidate:... ───────────│
       │                                 │                               │
       │  6. 执行连接性检查 (STUN Binding)│  6. 执行连接性检查              │
       │══════════ STUN Binding ═══════════════════ STUN Binding ═══════│
       │                                 │                               │
       │  7. 选择最优候选对，完成连接      │  7. 选择最优候选对，完成连接    │
       │                                 │                               │
       │══════════ 应用数据传输 ═══════════════════ 应用数据传输 ═════════│
```

### 3.2 STUN 协议

STUN（Session Traversal Utilities for NAT）是 ICE 的核心工具：

1. 客户端向 STUN 服务器发送 Binding Request
2. 服务器回复 Binding Response，其中包含 `XOR-MAPPED-ADDRESS` 属性
3. 该属性值即为客户端的公网 IP:Port（服务器反射地址）

```
Client                    STUN Server
  │                           │
  │── Binding Request ───────►│
  │                           │
  │◄─ Binding Response ───────│
  │   (XOR-MAPPED-ADDRESS     │
  │    = 公网IP:Port)          │
```

STUN 还用于 ICE 的连接性检查：双方互发 STUN Binding Request，验证对方的候选地址可达。

### 3.3 TURN 中继

当直接连接（P2P）无法建立时，使用 TURN 服务器进行中继：

```
Agent A                  TURN Server                 Agent B
  │                           │                         │
  │── Allocate Request ──────►│                         │
  │◄─ Allocate Response ──────│                         │
  │   (XOR-RELAYED-ADDRESS)   │                         │
  │                           │                         │
  │── Send (data) ───────────►│── Data Indication ─────►│
  │                           │                         │
  │◄─ Data Indication ────────│◄── Send (data) ────────│
```

TURN 中继的关键概念：

- **Allocation**：在 TURN 服务器上分配一个中继地址
- **Permission**：控制哪些远端地址可以发送数据到中继
- **Channel**：为频繁通信的远端建立通道绑定，避免每包都带 STUN 头

### 3.4 SDP 交互

ICE 使用 SDP（Session Description Protocol）交换连接信息：

```
a=ice-ufrag:xs8h
a=ice-pwd:gh7894gh7894gh7894gh
a=candidate:1 1 UDP 2130706431 192.168.1.100 50000 typ host
a=candidate:2 1 UDP 1694498815 203.0.113.1 50000 typ srflx
a=candidate:3 1 UDP 16777215 198.51.100.1 60000 typ relay
```

- `ice-ufrag` / `ice-pwd`：ICE 认证凭据
- 每行 `a=candidate:` 描述一个候选地址
- Trickle ICE 允许候选者逐步交换，无需等待全部收集完成

---

## 4. 协议报文格式

### STUN 消息头（20 字节）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|0 0|     STUN Message Type     |         Message Length        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic Cookie = 0x2112A442                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                     Transaction ID (96 bits)                  |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段             | 大小    | 说明                               |
| -------------- | ----- | -------------------------------- |
| Message Type   | 2 字节  | 消息类型（方法 + 类）                     |
| Message Length | 2 字节  | 消息体长度（不含头部）                      |
| Magic Cookie   | 4 字节  | 固定值 `0x2112A442`，用于区分 STUN 和其他协议 |
| Transaction ID | 12 字节 | 事务标识，请求与响应匹配                     |

### STUN 属性

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|             Type              |            Length             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Value (variable)                     ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### libjuice 支持的 STUN 属性

| 属性                         | 类型值    | 用途              |
| -------------------------- | ------ | --------------- |
| `MAPPED-ADDRESS`           | 0x0001 | 映射地址            |
| `USERNAME`                 | 0x0006 | 用户名             |
| `MESSAGE-INTEGRITY`        | 0x0008 | HMAC-SHA1 消息完整性 |
| `ERROR-CODE`               | 0x0009 | 错误码             |
| `REALM`                    | 0x0014 | 认证域             |
| `NONCE`                    | 0x0015 | 认证随机数           |
| `MESSAGE-INTEGRITY-SHA256` | 0x001C | HMAC-SHA256 完整性 |
| `PASSWORD-ALGORITHM`       | 0x001D | 密码算法            |
| `USERHASH`                 | 0x001E | 用户名哈希           |
| `XOR-MAPPED-ADDRESS`       | 0x0020 | XOR 映射地址        |
| `PRIORITY`                 | 0x0024 | 候选者优先级          |
| `USE-CANDIDATE`            | 0x0025 | 标记有效候选          |
| `ICE-CONTROLLED`           | 0x8029 | 被控方角色           |
| `ICE-CONTROLLING`          | 0x802A | 主控方角色           |
| `FINGERPRINT`              | 0x8028 | CRC32 指纹        |
| `CHANNEL-NUMBER`           | 0x000C | TURN 通道号        |
| `LIFETIME`                 | 0x000D | TURN 分配生命周期     |
| `XOR-PEER-ADDRESS`         | 0x0012 | TURN 对端地址       |
| `DATA`                     | 0x0013 | TURN 数据         |
| `XOR-RELAYED-ADDRESS`      | 0x0016 | TURN 中继地址       |
| `REQUESTED-TRANSPORT`      | 0x0019 | TURN 请求传输协议     |

### TURN ChannelData 消息

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Channel Number        |            Length             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                       Application Data                        /
|                                                               |
+-------------------------------+
```

ChannelData 相比 Send/Data Indication 消除了 STUN 头部开销，适用于频繁数据传输。

---

## 5. 架构设计

### 5.1 整体架构

```
┌──────────────────────────────────────────────────────┐
│                    应用层                              │
├──────────────────────────────────────────────────────┤
│              juice.h (公共 API)                       │
├──────────────────────────────────────────────────────┤
│                  juice.c (API 适配层)                  │
├──────────────────────────────────────────────────────┤
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│  │  agent.c │  │  ice.c   │  │ server.c │           │
│  │ ICE Agent│  │ICE 解析器│  │STUN/TURN │           │
│  │ 核心逻辑 │  │SDP 处理  │  │  服务器   │           │
│  └────┬─────┘  └──────────┘  └──────────┘           │
│       │                                              │
│  ┌────┴──────────────────────────────────┐           │
│  │            stun.c                      │           │
│  │    STUN 消息编解码、完整性校验          │           │
│  └────┬──────────────────────────────────┘           │
│       │                                              │
│  ┌────┴──────────────────────────────────┐           │
│  │    turn.c    │   conn.c / conn_*.c    │           │
│  │  TURN 中继   │   连接抽象层            │           │
│  │  状态管理    │ (poll/mux/thread)      │           │
│  └─────────────┴─────────────────────────┘           │
│                                                      │
│  ┌────────────────────────────────────────┐           │
│  │  udp.c  │  tcp.c  │  addr.c  │  hash.c │          │
│  │ UDP封装 │ TCP封装  │ 地址处理 │ 哈希函数│          │
│  └─────────┴─────────┴─────────┴─────────┘           │
│                                                      │
│  ┌────────────────────────────────────────┐           │
│  │ hmac.c │ crc32.c │ random.c │ base64.c │          │
│  │ HMAC   │  CRC32  │ 随机数   │  Base64  │          │
│  └────────┴─────────┴─────────┴──────────┘           │
│                                                      │
│  ┌────────────────────────────────────────┐           │
│  │  timestamp.c  │  log.c  │ const_time.c │          │
│  │   时间戳      │  日志   │ 常量时间比较  │          │
│  └───────────────┴─────────┴──────────────┘           │
└──────────────────────────────────────────────────────┘
```

### 5.2 模块划分

| 模块                | 文件                                                                 | 职责                                    |
| ----------------- | ------------------------------------------------------------------ | ------------------------------------- |
| **公共 API**        | `juice.h` / `juice.c`                                              | 对外接口，参数校验，调用内部实现                      |
| **ICE Agent**     | `agent.h` / `agent.c`                                              | ICE 核心状态机、候选者管理、连接性检查                 |
| **ICE 解析**        | `ice.h` / `ice.c`                                                  | SDP 解析/生成、候选者结构管理                     |
| **STUN 协议**       | `stun.h` / `stun.c`                                                | STUN 消息编解码、属性处理、完整性校验                 |
| **TURN 中继**       | `turn.h` / `turn.c`                                                | TURN 通道绑定、权限管理、ChannelData 处理         |
| **STUN/TURN 服务器** | `server.h` / `server.c`                                            | 服务端 STUN Binding 和 TURN Allocation 实现 |
| **连接抽象**          | `conn.h` / `conn.c`                                                | 连接模式分发（poll/mux/thread）               |
| **Poll 模式**       | `conn_poll.h` / `conn_poll.c`                                      | 所有连接共享一个线程的 poll 模型                   |
| **Mux 模式**        | `conn_mux.h` / `conn_mux.c`                                        | 所有连接复用一个 UDP socket                   |
| **Thread 模式**     | `conn_thread.h` / `conn_thread.c`                                  | 每个连接独占一个线程                            |
| **UDP 封装**        | `udp.h` / `udp.c`                                                  | 跨平台 UDP socket 操作                     |
| **TCP 封装**        | `tcp.h` / `tcp.c`                                                  | ICE-TCP 的 TCP 连接管理                    |
| **地址处理**          | `addr.h` / `addr.c`                                                | 跨平台地址解析、sockaddr 操作                   |
| **密码学**           | `hmac.h` / `hash.h` / `crc32.c`                                    | HMAC-SHA1/SHA256、CRC32、MD5            |
| **工具**            | `random.c` / `base64.c` / `timestamp.c` / `log.c` / `const_time.c` | 随机数、Base64、时间戳、日志、常量时间比较              |

### 5.3 并发模型

libjuice 提供三种并发模式，通过 `juice_concurrency_mode_t` 配置：

| 模式         | 枚举值                             | 说明              | 适用场景      |
| ---------- | ------------------------------- | --------------- | --------- |
| **Poll**   | `JUICE_CONCURRENCY_MODE_POLL`   | 所有连接共享一个线程      | 少量连接，资源受限 |
| **Mux**    | `JUICE_CONCURRENCY_MODE_MUX`    | 所有连接复用一个 UDP 端口 | 大量连接，端口受限 |
| **Thread** | `JUICE_CONCURRENCY_MODE_THREAD` | 每个连接独占一个线程      | 高吞吐，多核系统  |

连接抽象层（`conn.c`）通过函数指针表 `conn_mode_entry_t` 实现策略模式：

```c
typedef struct conn_mode_entry {
    int (*registry_init_func)(conn_registry_t *registry, udp_socket_config_t *config);
    void (*registry_cleanup_func)(conn_registry_t *registry);
    int (*init_func)(juice_agent_t *agent, ...);
    void (*cleanup_func)(juice_agent_t *agent);
    void (*lock_func)(juice_agent_t *agent);
    void (*unlock_func)(juice_agent_t *agent);
    int (*send_func)(juice_agent_t *agent, ...);
    // ...
} conn_mode_entry_t;
```

### 5.4 核心数据结构

#### juice_agent_t — ICE Agent

```c
struct juice_agent {
    juice_config_t config;          // 用户配置
    juice_state_t state;            // 当前状态
    agent_mode_t mode;              // controlling/controlled
    juice_ice_tcp_mode_t ice_tcp_mode;

    ice_description_t local;        // 本地 ICE 描述
    ice_description_t remote;       // 远端 ICE 描述

    ice_candidate_pair_t candidate_pairs[MAX_CANDIDATE_PAIRS_COUNT];
    ice_candidate_pair_t *ordered_pairs[MAX_CANDIDATE_PAIRS_COUNT];
    ice_candidate_pair_t *selected_pair;  // 最终选中的候选对
    int candidate_pairs_count;

    agent_stun_entry_t entries[MAX_STUN_ENTRIES_COUNT];
    int entries_count;
    atomic_ptr(agent_stun_entry_t) selected_entry;

    uint64_t ice_tiebreaker;        // 角色冲突解决
    timestamp_t pac_timestamp;      // PAC 定时器
    timestamp_t nomination_timestamp;
    bool gathering_done;

    conn_registry_t *registry;      // 连接注册表
    int conn_index;
    void *conn_impl;                // 连接模式实现
};
```

#### ice_candidate_t — 候选地址

```c
typedef struct ice_candidate {
    ice_candidate_type_t type;      // host/srflx/prflx/relayed
    uint32_t priority;              // 优先级
    int component;                  // 组件 ID
    char foundation[32 + 1];        // 基础标识
    ice_candidate_transport_t transport; // UDP/TCP
    char hostname[256 + 1];         // 主机名
    char service[32 + 1];           // 端口号（字符串）
    addr_record_t resolved;         // 解析后的地址
} ice_candidate_t;
```

#### ice_candidate_pair_t — 候选对

```c
typedef struct ice_candidate_pair {
    ice_candidate_t *local;
    ice_candidate_t *remote;
    uint64_t priority;
    ice_candidate_pair_state_t state; // pending/succeeded/failed/frozen
    bool nominated;
    bool nomination_requested;
    timestamp_t consent_expiry;     // Consent Freshness 过期时间
} ice_candidate_pair_t;
```

#### agent_stun_entry_t — STUN 事务条目

```c
typedef struct agent_stun_entry {
    agent_stun_entry_type_t type;   // server/relay/check
    agent_stun_entry_state_t state; // pending/succeeded/failed/...
    agent_mode_t mode;
    ice_candidate_pair_t *pair;
    addr_record_t record;
    uint8_t transaction_id[STUN_TRANSACTION_ID_SIZE];
    timestamp_t next_transmission;
    timediff_t retransmission_timeout;
    int retransmissions;
    // TURN 相关
    agent_turn_state_t *turn;
    struct agent_stun_entry *relay_entry;
} agent_stun_entry_t;
```

---

## 6. 实现详解

### 6.1 ICE Agent 生命周期

libjuice 的 ICE Agent 经历以下状态转换：

```
DISCONNECTED ──► GATHERING ──► CONNECTING ──► CONNECTED ──► COMPLETED
                    │               │                          │
                    │               └──────────────────────► FAILED
                    └──────────────────────────────────────► FAILED
```

| 状态                         | 说明             |
| -------------------------- | -------------- |
| `JUICE_STATE_DISCONNECTED` | 初始状态，未开始收集     |
| `JUICE_STATE_GATHERING`    | 正在收集候选地址       |
| `JUICE_STATE_CONNECTING`   | 正在执行连接性检查      |
| `JUICE_STATE_CONNECTED`    | 找到可用候选对，可以发送数据 |
| `JUICE_STATE_COMPLETED`    | 所有检查完成，选出最优对   |
| `JUICE_STATE_FAILED`       | 连接失败           |

### 6.2 候选者收集

`agent_gather_candidates()` 的执行流程：

1. **主机候选**：枚举本地网络接口地址
2. **服务器反射候选**：向配置的 STUN 服务器发送 Binding Request
3. **中继候选**：向配置的 TURN 服务器发送 Allocate Request
4. **ICE-TCP 候选**：如果启用，添加 TCP 活动候选
5. 每发现一个候选者，通过 `cb_candidate` 回调通知用户（Trickle ICE）
6. 收集完成后，触发 `cb_gathering_done` 回调

**限制**：最多 2 个 STUN 服务器、2 个 TURN 服务器、30 个候选者。

### 6.3 连接性检查

连接性检查是 ICE 的核心过程：

1. 为每对本地-远端候选创建候选对（`ice_candidate_pair_t`）
2. 按优先级排序候选对
3. 对每个候选对发送 STUN Binding Request（带 `PRIORITY` 和 `ICE-CONTROLLING/CONTROLLED` 属性）
4. 收到成功响应则标记该对为 `SUCCEEDED`
5. Controlling Agent 选择最优对并标记 `NOMINATED`

**重传策略**（遵循 RFC 8445）：

```
初始 RTO = 500ms
指数退避：500ms → 1s → 2s → 4s → 8s → 16s
最大重传次数 = 6（总计 39500ms）
```

### 6.4 候选者对选择

Controlling Agent 负责选择最优候选对：

1. 所有候选对按优先级排序
2. 优先选择 `SUCCEEDED` 状态的对
3. 在成功对中，优先级最高的被 `NOMINATED`
4. 被提名对经过对方确认后成为 `selected_pair`
5. 设置 2 秒的 nomination 超时，防止无限等待

优先级计算（RFC 8445）：

```
pair priority = 2^32 * min(G, D) + 2 * max(G, D) + (G > D ? 1 : 0)
```

其中 G 为 controlling 侧优先级，D 为 controlled 侧优先级。

### 6.5 STUN 消息处理

libjuice 的 STUN 处理流程：

**发送（`stun_write`）**：

1. 填充 STUN 头部（类型、长度、Magic Cookie、Transaction ID）
2. 逐个写入属性
3. 计算 `MESSAGE-INTEGRITY`（HMAC-SHA1 或 HMAC-SHA256）
4. 计算 `FINGERPRINT`（CRC32）

**接收（`stun_read`）**：

1. 验证 Magic Cookie
2. 解析头部
3. 逐个解析属性
4. 校验 `MESSAGE-INTEGRITY`（`stun_check_integrity`）
5. 校验 `FINGERPRINT`

**消息分派（`agent_dispatch_stun`）**：

```
收到 STUN 消息
      │
      ├── Binding Request → agent_process_stun_binding()
      │     ├── 验证凭据
      │     ├── 发现 peer reflexive 候选
      │     └── 发送 Binding Response
      │
      ├── Binding Response → agent_process_stun_binding()
      │     ├── 更新候选对状态
      │     ├── 发现 peer reflexive 候选
      │     └── 触发 nomination
      │
      ├── Allocate Response → agent_process_turn_allocate()
      │
      ├── CreatePermission Response → agent_process_turn_create_permission()
      │
      ├── ChannelBind Response → agent_process_turn_channel_bind()
      │
      └── Data Indication → agent_process_turn_data()
```

**STUN/应用数据多路复用**：

libjuice 通过 `is_stun_datagram()` 判断收到的 UDP 包是 STUN 消息还是应用数据：

- STUN 消息：前两位为 00，Magic Cookie 为 `0x2112A442`
- ChannelData：Channel 号范围 `0x4000-0x7FFF`
- 应用数据：其他情况

### 6.6 TURN 中继实现

libjuice 实现了完整的 TURN 客户端：

**分配流程**：

1. 发送 Allocate Request（`REQUESTED-TRANSPORT = UDP(17)`）
2. 服务器返回 401（需要认证）+ `NONCE` + `REALM`
3. 使用长期凭据机制重新发送 Allocate Request
4. 服务器返回 `XOR-RELAYED-ADDRESS`（中继地址）

**权限管理**：

- 使用 `turn_map_t` 管理权限和通道绑定
- `CreatePermission` 请求控制对端可达性
- 权限生命周期 = 300 秒（RFC 8656）

**通道绑定**：

- `ChannelBind` 请求建立通道号与对端地址的映射
- 通道号范围：`0x4000-0x7FFF`
- 通道绑定生命周期 = 600 秒（RFC 8656）
- 绑定后可使用 ChannelData 消息格式，减少开销

**数据发送**：

- 有通道绑定 → `agent_channel_send()`（ChannelData 格式）
- 无通道绑定 → `agent_relay_send()`（Send Indication 格式）

### 6.7 ICE-TCP 支持

libjuice 支持 RFC 6544 定义的 ICE-TCP：

- 模式：`JUICE_ICE_TCP_MODE_NONE`（默认禁用）或 `JUICE_ICE_TCP_MODE_ACTIVE`
- Active 模式：作为 TCP 客户端主动连接
- TCP 候选优先级降低 50（`ICE_CANDIDATE_PENALTY_TCP = 50`）

### 6.8 Consent Freshness

RFC 7675 定义的 Consent Freshness 机制确保远端仍然同意接收数据：

- 每 4-6 秒发送一次 STUN Binding Request（consent 检查）
- 如果 30 秒内未收到响应，consent 过期
- consent 过期后停止发送数据
- 可通过编译选项 `DISABLE_CONSENT_FRESHNESS` 关闭

```c
#define CONSENT_TIMEOUT 30000           // 30 秒
#define MIN_CONSENT_CHECK_PERIOD 4000   // 最小 4 秒
#define MAX_CONSENT_CHECK_PERIOD 6000   // 最大 6 秒
```

### 6.9 PAC 定时器

PAC（Patiently Awaiting Connectivity）定时器是 RFC 8863 的实现：

- 在开始连接性检查前等待一段时间
- 给予 Trickle ICE 更多时间收集候选者
- 避免在候选者不完整时就开始检查
- 超时时间 = 39500ms（等于完整的 STUN 重传序列）

### 6.10 STUN/TURN 服务器端实现

libjuice 内置了一个轻量级 STUN/TURN 服务器：

```c
typedef struct juice_server_config {
    juice_server_credentials_t *credentials;
    int credentials_count;
    int max_allocations;          // 默认 1000
    int max_peers;                // 默认 16
    const char *bind_address;
    const char *external_address; // 外部地址（NAT 后需要设置）
    uint16_t port;
    uint16_t relay_port_range_begin;
    uint16_t relay_port_range_end;
    const char *realm;            // 默认 "libjuice"
} juice_server_config_t;
```

服务器特性：

- 支持 STUN Binding 和 TURN Allocation
- 长期凭据认证机制
- 动态添加凭据（`juice_server_add_credentials`）
- Nonce 密钥每 10 分钟轮换
- 可通过 `NO_SERVER` 编译选项禁用

---

## 7. 特性与限制

### 已实现的特性

| 特性                | RFC 标准   | 说明                |
| ----------------- | -------- | ----------------- |
| Full ICE Agent    | RFC 8445 | 完整 ICE 代理实现       |
| STUN 协议           | RFC 8489 | STUN 消息编解码        |
| TURN 中继           | RFC 8656 | TURN 客户端          |
| TCP 候选            | RFC 6544 | ICE-TCP Active 模式 |
| Consent Freshness | RFC 7675 | 同意新鲜度检查           |
| PAC 定时器           | RFC 8863 | 耐心等待连接性           |
| SDP 接口            | RFC 8839 | SDP 解析与生成         |
| Trickle ICE       | -        | 增量候选者交换           |
| IPv4/IPv6 双栈      | -        | 同时支持两种地址族         |
| 端口复用              | -        | 多连接共享一个 UDP 端口    |
| DiffServ          | -        | 发送时设置 DSCP 标记     |
| STUN/TURN 服务器     | -        | 内置轻量服务器           |

### 已知限制

| 限制                | 说明                                     |
| ----------------- | -------------------------------------- |
| 仅支持 UDP 传输        | 其他传输协议被忽略                              |
| 仅支持单个组件           | 足以覆盖 WebRTC Data Channel 和 RTP/RTCP 复用 |
| 仅支持 RFC 8828 模式 2 | 默认路由 + 所有本地地址                          |
| 最多 2 个 STUN 服务器   | `MAX_SERVER_ENTRIES_COUNT = 2`         |
| 最多 2 个 TURN 服务器   | `MAX_RELAY_ENTRIES_COUNT = 2`          |
| 最多 30 个候选者        | `ICE_MAX_CANDIDATES_COUNT = 30`        |

---

## 8. API 参考

### ICE Agent 接口

| 函数                                                  | 说明              |
| --------------------------------------------------- | --------------- |
| `juice_create(config)`                              | 创建 ICE Agent    |
| `juice_destroy(agent)`                              | 销毁 ICE Agent    |
| `juice_gather_candidates(agent)`                    | 开始收集候选者         |
| `juice_get_local_description(agent, buf, size)`     | 获取本地 SDP 描述     |
| `juice_set_remote_description(agent, sdp)`          | 设置远端 SDP 描述     |
| `juice_add_remote_candidate(agent, sdp)`            | 添加远端候选者         |
| `juice_add_turn_server(agent, turn_server)`         | 动态添加 TURN 服务器   |
| `juice_set_remote_gathering_done(agent)`            | 通知远端候选者收集完成     |
| `juice_send(agent, data, size)`                     | 发送数据            |
| `juice_send_diffserv(agent, data, size, ds)`        | 带 DiffServ 标记发送 |
| `juice_get_state(agent)`                            | 获取当前状态          |
| `juice_get_selected_candidates(agent, ...)`         | 获取选中的候选对        |
| `juice_get_selected_addresses(agent, ...)`          | 获取选中的地址对        |
| `juice_set_local_ice_attributes(agent, ufrag, pwd)` | 自定义 ICE 凭据      |
| `juice_set_ice_tcp_mode(agent, mode)`               | 设置 ICE-TCP 模式   |
| `juice_state_to_string(state)`                      | 状态转字符串          |

### ICE Server 接口

| 函数                                                      | 说明               |
| ------------------------------------------------------- | ---------------- |
| `juice_server_create(config)`                           | 创建 STUN/TURN 服务器 |
| `juice_server_destroy(server)`                          | 销毁服务器            |
| `juice_server_get_port(server)`                         | 获取监听端口           |
| `juice_server_add_credentials(server, creds, lifetime)` | 动态添加凭据           |

### Mux 监听接口

| 函数                                                | 说明          |
| ------------------------------------------------- | ----------- |
| `juice_mux_listen(bind_addr, port, cb, user_ptr)` | 在复用端口上监听新连接 |

### 日志接口

| 函数                           | 说明       |
| ---------------------------- | -------- |
| `juice_set_log_level(level)` | 设置日志级别   |
| `juice_set_log_handler(cb)`  | 自定义日志处理器 |

### 回调函数类型

| 回调                          | 触发时机                 |
| --------------------------- | -------------------- |
| `juice_cb_state_changed_t`  | 状态变化时                |
| `juice_cb_candidate_t`      | 发现新候选者时（Trickle ICE） |
| `juice_cb_gathering_done_t` | 候选者收集完成时             |
| `juice_cb_recv_t`           | 收到应用数据时              |

### 错误码

| 错误码                   | 值   | 说明    |
| --------------------- | --- | ----- |
| `JUICE_ERR_SUCCESS`   | 0   | 成功    |
| `JUICE_ERR_INVALID`   | -1  | 无效参数  |
| `JUICE_ERR_FAILED`    | -2  | 运行时错误 |
| `JUICE_ERR_NOT_AVAIL` | -3  | 元素不可用 |
| `JUICE_ERR_IGNORED`   | -4  | 被忽略   |
| `JUICE_ERR_AGAIN`     | -5  | 缓冲区满  |
| `JUICE_ERR_TOO_LARGE` | -6  | 数据报过大 |

---

## 9. 使用示例

### 基本连接示例

```c
#include <juice/juice.h>
#include <stdio.h>
#include <string.h>

static juice_agent_t *agent1;
static juice_agent_t *agent2;

// 状态变化回调
static void on_state_changed1(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
    printf("Agent 1 state: %s\n", juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED) {
        juice_send(agent, "Hello from 1", 13);
    }
}

static void on_state_changed2(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
    printf("Agent 2 state: %s\n", juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED) {
        juice_send(agent, "Hello from 2", 13);
    }
}

// 候选者发现回调
static void on_candidate1(juice_agent_t *agent, const char *sdp, void *user_ptr) {
    printf("Candidate 1: %s\n", sdp);
    juice_add_remote_candidate(agent2, sdp);  // 传给另一端
}

static void on_candidate2(juice_agent_t *agent, const char *sdp, void *user_ptr) {
    printf("Candidate 2: %s\n", sdp);
    juice_add_remote_candidate(agent1, sdp);  // 传给另一端
}

// 收集完成回调
static void on_gathering_done1(juice_agent_t *agent, void *user_ptr) {
    juice_set_remote_gathering_done(agent2);
}

static void on_gathering_done2(juice_agent_t *agent, void *user_ptr) {
    juice_set_remote_gathering_done(agent1);
}

// 数据接收回调
static void on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
    char buf[4096];
    size_t len = size < 4095 ? size : 4095;
    memcpy(buf, data, len);
    buf[len] = '\0';
    printf("Received: %s\n", buf);
}

int main() {
    // 配置 Agent 1
    juice_config_t config1 = {0};
    config1.stun_server_host = "stun.l.google.com";
    config1.stun_server_port = 19302;
    config1.cb_state_changed = on_state_changed1;
    config1.cb_candidate = on_candidate1;
    config1.cb_gathering_done = on_gathering_done1;
    config1.cb_recv = on_recv;

    agent1 = juice_create(&config1);

    // 配置 Agent 2
    juice_config_t config2 = {0};
    config2.stun_server_host = "stun.l.google.com";
    config2.stun_server_port = 19302;
    config2.cb_state_changed = on_state_changed2;
    config2.cb_candidate = on_candidate2;
    config2.cb_gathering_done = on_gathering_done2;
    config2.cb_recv = on_recv;

    agent2 = juice_create(&config2);

    // 交换 SDP 描述
    char sdp1[JUICE_MAX_SDP_STRING_LEN];
    juice_get_local_description(agent1, sdp1, sizeof(sdp1));
    juice_set_remote_description(agent2, sdp1);

    char sdp2[JUICE_MAX_SDP_STRING_LEN];
    juice_get_local_description(agent2, sdp2, sizeof(sdp2));
    juice_set_remote_description(agent1, sdp2);

    // 开始收集候选者
    juice_gather_candidates(agent1);
    juice_gather_candidates(agent2);

    // 等待连接完成...

    // 清理
    juice_destroy(agent1);
    juice_destroy(agent2);
    return 0;
}
```

### 使用 TURN 服务器

```c
juice_config_t config = {0};
config.stun_server_host = "stun.example.com";
config.stun_server_port = 3478;

// 配置 TURN 服务器
juice_turn_server_t turn_server = {
    .host = "turn.example.com",
    .port = 3478,
    .username = "user",
    .password = "pass"
};
config.turn_servers = &turn_server;
config.turn_servers_count = 1;

juice_agent_t *agent = juice_create(&config);
```

### 创建 STUN/TURN 服务器

```c
juice_server_credentials_t creds[] = {
    { .username = "user1", .password = "pass1", .allocations_quota = 10 },
    { .username = "user2", .password = "pass2", .allocations_quota = 5 },
};

juice_server_config_t server_config = {
    .credentials = creds,
    .credentials_count = 2,
    .port = 3478,
    .realm = "myapp",
};

juice_server_t *server = juice_server_create(&server_config);

// 动态添加凭据
juice_server_credentials_t new_cred = {
    .username = "user3",
    .password = "pass3",
    .allocations_quota = 10,
};
juice_server_add_credentials(server, &new_cred, 3600000);  // 1 小时有效期
```

---

## 10. 编译与集成

### CMake 构建

```bash
# 标准构建
cmake -B build
cd build
make -j$(nproc)

# 使用 Nettle 提供加密
cmake -B build -DUSE_NETTLE=1

# 禁用服务器支持（减小体积）
cmake -B build -DNO_SERVER=1

# 禁用测试
cmake -B build -DNO_TESTS=1
```

### 集成到 CMake 项目

```cmake
# 方式 1：子目录
add_subdirectory(libjuice)
target_link_libraries(myapp LibJuice::LibJuice)

# 方式 2：find_package
find_package(LibJuice REQUIRED)
target_link_libraries(myapp LibJuice::LibJuice)
```

### vcpkg 安装

```bash
vcpkg install libjuice
```

### 编译选项

| 选项                                 | 默认  | 说明                   |
| ---------------------------------- | --- | -------------------- |
| `BUILD_SHARED_LIBS`                | ON  | 构建共享库                |
| `USE_NETTLE`                       | OFF | 使用 Nettle 替代内置哈希     |
| `NO_SERVER`                        | OFF | 禁用服务器支持              |
| `NO_TESTS`                         | OFF | 禁用测试                 |
| `WARNINGS_AS_ERRORS`               | OFF | 警告视为错误               |
| `FUZZER`                           | OFF | 启用 fuzz 测试           |
| `DISABLE_CONSENT_FRESHNESS`        | OFF | 禁用 Consent Freshness |
| `ENABLE_LOCALHOST_ADDRESS`         | OFF | 在候选者中列出 localhost    |
| `ENABLE_LOCAL_ADDRESS_TRANSLATION` | OFF | 将本地地址转换为 localhost   |

### 平台依赖

| 平台      | 系统库            |
| ------- | -------------- |
| Linux   | pthread        |
| Windows | ws2_32, bcrypt |
| macOS   | 无额外依赖          |

---

## 11. 与 libnice 的详细比较

### 11.1 基本信息对比

| 维度             | libjuice                             | libnice                                |
| -------------- | ------------------------------------ | -------------------------------------- |
| **仓库**         | github.com/paullouisageneau/libjuice | gitlab.freedesktop.org/libnice/libnice |
| **语言**         | C (C11)                              | C (GLib 风格)                            |
| **许可证**        | MPL 2.0                              | LGPL/MPL                               |
| **依赖**         | **零依赖**                              | GLib（必须）                               |
| **代码规模**       | ~5000 行                              | ~30000+ 行                              |
| **公共 API 头文件** | 1 个 (`juice.h`)                      | 多个                                     |
| **维护状态**       | 活跃                                   | 活跃（GStreamer 社区维护）                     |

### 11.2 功能对比

| 功能                | libjuice             | libnice                      |
| ----------------- | -------------------- | ---------------------------- |
| ICE Full Agent    | ✅ (简化)               | ✅ (完整)                       |
| ICE Lite Agent    | ❌                    | ✅                            |
| STUN 客户端          | ✅ (RFC 8489)         | ✅ (RFC 5389/8489)            |
| TURN 客户端          | ✅ (RFC 8656)         | ✅ (RFC 5766/8656)            |
| STUN/TURN 服务器     | ✅ 内置                 | ❌                            |
| ICE-TCP           | ✅ (RFC 6544, Active) | ✅ (RFC 6544, Active+Passive) |
| 多组件支持             | ❌ (仅 1 个)            | ✅ (多组件)                      |
| Trickle ICE       | ✅                    | ✅                            |
| Consent Freshness | ✅ (RFC 7675)         | ✅                            |
| PAC 定时器           | ✅ (RFC 8863)         | ✅                            |
| IPv4/IPv6 双栈      | ✅                    | ✅                            |
| 端口复用 (Mux)        | ✅                    | ❌                            |
| DiffServ 标记       | ✅                    | ❌                            |
| Pseudo TCP        | ❌                    | ✅ (内置)                       |
| GStreamer 集成      | ❌                    | ✅ (原生)                       |
| SDP 接口            | ✅                    | ✅                            |

### 11.3 架构对比

| 维度       | libjuice                | libnice            |
| -------- | ----------------------- | ------------------ |
| **设计风格** | 简化、自包含                  | 完整、模块化             |
| **对象模型** | 结构体 + 函数                | GObject (GLib)     |
| **事件循环** | 内置 poll / 自定义线程         | GLib MainLoop      |
| **内存管理** | calloc/free             | GLib 内存管理 + 引用计数   |
| **线程模型** | 3 种可选 (poll/mux/thread) | GLib 线程 + MainLoop |
| **日志系统** | 内置，可自定义                 | GLib 日志            |
| **代码组织** | 扁平，文件间耦合低               | 分层，GLib 风格模块化      |

### 11.4 API 风格对比

**libjuice** — 简洁的过程式 API：

```c
// 创建
juice_agent_t *agent = juice_create(&config);

// 使用
juice_gather_candidates(agent);
juice_send(agent, data, size);

// 销毁
juice_destroy(agent);
```

**libnice** — GObject 风格 API：

```c
// 创建
NiceAgent *agent = nice_agent_new(g_main_loop_get_context(loop),
                                   NICE_COMPATIBILITY_RFC5245);

// 使用（属性 + 信号）
g_signal_connect(agent, "candidate-gathering-done", ...);
nice_agent_gather_candidates(agent, stream_id);

// 销毁
g_object_unref(agent);
```

### 11.5 适用场景对比

| 场景            | 推荐           | 原因                         |
| ------------- | ------------ | -------------------------- |
| 嵌入式/物联网       | **libjuice** | 零依赖，体积小                    |
| 游戏引擎集成        | **libjuice** | 简单 API，无 GLib 依赖           |
| WebRTC 客户端    | 两者皆可         | libjuice 足以覆盖 Data Channel |
| GStreamer 管道  | **libnice**  | 原生 GStreamer 集成            |
| 需要 Pseudo TCP | **libnice**  | 内置 Pseudo TCP 实现           |
| 多媒体流（多组件）     | **libnice**  | 支持多组件（RTP + RTCP 分离）       |
| 服务器端 TURN     | **libjuice** | 内置 STUN/TURN 服务器           |
| 大规模连接         | **libjuice** | Mux 模式，端口复用                |
| 需要 ICE Lite   | **libnice**  | libjuice 不支持 ICE Lite      |
| 快速原型          | **libjuice** | API 简单，上手快                 |

### 11.6 优缺点总结

**libjuice 优势**：

- 零依赖，极轻量
- API 简洁直观
- 内置 STUN/TURN 服务器
- 端口复用（Mux 模式）
- DiffServ 支持
- 更现代的 RFC 遵循
- 交叉编译友好
- Windows 原生支持

**libjuice 劣势**：

- 仅支持单组件
- 不支持 ICE Lite
- 不支持 TCP Passive/SO 模式
- 无 Pseudo TCP
- 无 GStreamer 集成
- 社区较小

**libnice 优势**：

- 完整 ICE 实现
- 多组件支持
- ICE Lite 支持
- Pseudo TCP 内置
- GStreamer 原生集成
- 成熟稳定，大规模部署验证

**libnice 劣势**：

- 强制依赖 GLib
- API 复杂（GObject）
- 代码量大
- 无内置 STUN/TURN 服务器
- 无端口复用模式
- 交叉编译较复杂

---

## 12. 最佳实践

### 12.1 STUN/TURN 服务器选择

- 本地测试：使用 Google STUN（`stun.l.google.com:19302`）
- 生产环境：部署自建 STUN/TURN 服务器（可使用 libjuice 内置服务器或 [Violet](https://github.com/paullouisageneau/violet)）
- TURN 服务器必须用于 Symmetric NAT 穿透

### 12.2 并发模式选择

| 场景        | 推荐模式   |
| --------- | ------ |
| 嵌入式设备     | Poll   |
| 服务器（大量连接） | Mux    |
| 高吞吐场景     | Thread |

### 12.3 ICE 凭据安全

- `ice-ufrag` 最少 4 个字符，`ice-pwd` 最少 22 个字符
- 可通过 `juice_set_local_ice_attributes()` 自定义
- 每次会话应使用不同的凭据

### 12.4 错误处理

```c
int ret = juice_send(agent, data, size);
if (ret == JUICE_ERR_AGAIN) {
    // 缓冲区满，稍后重试
} else if (ret == JUICE_ERR_TOO_LARGE) {
    // 数据报过大，需要分片
} else if (ret < 0) {
    // 其他错误
}
```

### 12.5 资源管理

- 及时调用 `juice_destroy()` 释放 Agent
- 服务器端设置合理的 `max_allocations` 和 `allocations_quota`
- 使用 `juice_get_state()` 监控连接状态

---

## 13. 生态与衍生项目

| 项目                                                                       | 说明                                  |
| ------------------------------------------------------------------------ | ----------------------------------- |
| [Violet](https://github.com/paullouisageneau/violet)                     | 基于 libjuice 的 STUN/TURN 服务器应用       |
| [juice-rs](https://github.com/VollmondT/juice-rs)                        | Rust 语言绑定                           |
| [datachannel-wasm](https://github.com/paullouisageneau/datachannel-wasm) | WebAssembly 版本                      |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel)     | 基于 libjuice 的 WebRTC Data Channel 库 |
| [AUR](https://aur.archlinux.org/packages/libjuice/)                      | Arch Linux 用户仓库包                    |
| [vcpkg](https://vcpkg.io/en/getting-started)                             | vcpkg 包管理器                          |

---

## 14. 常见问题

### Q1：libjuice 能穿透 Symmetric NAT 吗？

单独使用 STUN 无法穿透 Symmetric NAT。需要配置 TURN 服务器进行中继。

### Q2：为什么只支持单组件？

单组件足以覆盖绝大多数现代场景：WebRTC Data Channel 和 RTP/RTCP 复用（rtcp-mux）都只需要一个组件。多组件支持会显著增加复杂度。

### Q3：Mux 模式的工作原理？

多个 Agent 共享同一个 UDP socket，通过 STUN 消息中的 `ufrag` 字段路由到对应的 Agent。`juice_mux_listen()` 可以在此 socket 上监听新连接。

### Q4：如何自定义 ICE 凭据？

```c
juice_set_local_ice_attributes(agent, "my-ufrag", "my-very-long-password-12345");
```

必须在 `juice_get_local_description()` 之前调用。

### Q5：libjuice 是否线程安全？

Agent 本身不是线程安全的，但不同并发模式提供了不同的线程管理方式。跨 Agent 操作（如将一个 Agent 的候选者传给另一个）需要注意同步。

### Q6：如何检测连接断开？

- Consent Freshness 机制：远端 30 秒不响应 consent 检查，状态转为 `FAILED`
- 应用层心跳：定期发送心跳数据，检测超时

### Q7：可以禁用 Consent Freshness 吗？

可以，编译时添加 `-DDISABLE_CONSENT_FRESHNESS=1`，但**不推荐**，因为违反 RFC 7675。

### Q8：ICE-TCP 的用途？

在 UDP 被阻塞的网络环境中，TCP 可以作为后备传输。ICE-TCP 候选优先级低于 UDP，只在 UDP 不可用时使用。

---

## 15. 附录：源码常量定义

### STUN 重传

```c
MIN_STUN_RETRANSMISSION_TIMEOUT     500    // ms，最小 RTO
LAST_STUN_RETRANSMISSION_TIMEOUT    8000   // ms，最终 RTO
MAX_STUN_CHECK_RETRANSMISSION_COUNT 6      // 连接性检查最大重传次数
MAX_STUN_SERVER_RETRANSMISSION_COUNT 5     // 服务器查询最大重传次数
STUN_PACING_TIME                    50     // ms，STUN 间隔
STUN_KEEPALIVE_PERIOD               15000  // ms，Keepalive 间隔
```

### ICE 定时

```c
ICE_PAC_TIMEOUT                     39500  // ms，PAC 定时器
CONSENT_TIMEOUT                     30000  // ms，Consent 过期
MIN_CONSENT_CHECK_PERIOD            4000   // ms
MAX_CONSENT_CHECK_PERIOD            6000   // ms
NOMINATION_TIMEOUT                  2000   // ms，提名超时
```

### TURN 定时

```c
TURN_LIFETIME                       600000 // ms (10 min)
TURN_REFRESH_PERIOD                 540000 // ms (lifetime - 1 min)
PERMISSION_LIFETIME                 300000 // ms (5 min)
BIND_LIFETIME                       600000 // ms (10 min)
```

### 候选者限制

```c
ICE_MAX_CANDIDATES_COUNT            30     // 最大候选者数
MAX_SERVER_ENTRIES_COUNT            2      // 最大 STUN 服务器数
MAX_RELAY_ENTRIES_COUNT             2      // 最大 TURN 服务器数
MAX_HOST_CANDIDATES_COUNT           14     // 最大主机候选数
MAX_PEER_REFLEXIVE_CANDIDATES_COUNT 14    // 最大对端反射候选数
MAX_CANDIDATE_PAIRS_COUNT           90     // 最大候选对数
MAX_STUN_ENTRIES_COUNT              92     // 最大 STUN 条目数
```

### 候选者优先级

```c
ICE_CANDIDATE_PREF_HOST             126    // 主机候选
ICE_CANDIDATE_PREF_PEER_REFLEXIVE   110    // 对端反射候选
ICE_CANDIDATE_PREF_SERVER_REFLEXIVE 100    // 服务器反射候选
ICE_CANDIDATE_PREF_RELAYED          0      // 中继候选
ICE_CANDIDATE_PENALTY_TCP           50     // TCP 候选降权
```

### 服务器默认值

```c
SERVER_DEFAULT_REALM               "libjuice"
SERVER_DEFAULT_MAX_ALLOCATIONS     1000
SERVER_DEFAULT_MAX_PEERS           16
SERVER_NONCE_KEY_LIFETIME          600000  // ms (10 min)
```

---

## 参考资料

- [libjuice GitHub 仓库](https://github.com/paullouisageneau/libjuice)
- [RFC 8445 - ICE](https://www.rfc-editor.org/rfc/rfc8445.html)
- [RFC 8489 - STUN](https://www.rfc-editor.org/rfc/rfc8489.html)
- [RFC 8656 - TURN](https://www.rfc-editor.org/rfc/rfc8656.html)
- [RFC 6544 - ICE-TCP](https://www.rfc-editor.org/rfc/rfc6544.html)
- [RFC 7675 - Consent Freshness](https://www.rfc-editor.org/rfc/rfc7675.html)
- [RFC 8863 - PAC](https://www.rfc-editor.org/rfc/rfc8863.html)
- [RFC 8839 - SDP](https://www.rfc-editor.org/rfc/rfc8839.html)
