# KCP 协议深度解析：从原理到实践

> 基于官方仓库 [skywind3000/kcp](https://github.com/skywind3000/kcp) 撰写  
> 作者：林伟 (skywind3000) | 开源协议：MIT License

---

## 目录

1. [概述](#1-概述)
2. [背景知识：为什么需要 KCP](#2-背景知识为什么需要-kcp)
3. [核心原理](#3-核心原理)
   - 3.1 [ARQ 协议基础](#31-arq-协议基础)
   - 3.2 [TCP 的延迟瓶颈](#32-tcp-的延迟瓶颈)
   - 3.3 [KCP 的六大优化策略](#33-kcp-的六大优化策略)
4. [协议报文格式](#4-协议报文格式)
5. [架构设计](#5-架构设计)
   - 5.1 [整体架构](#51-整体架构)
   - 5.2 [核心数据结构](#52-核心数据结构)
   - 5.3 [四大队列](#53-四大队列)
   - 5.4 [数据流转](#54-数据流转)
6. [实现详解](#6-实现详解)
   - 6.1 [创建与销毁](#61-创建与销毁)
   - 6.2 [数据发送流程](#62-数据发送流程)
   - 6.3 [数据接收流程](#63-数据接收流程)
   - 6.4 [ACK 处理机制](#64-ack-处理机制)
   - 6.5 [RTO 计算与超时重传](#65-rto-计算与超时重传)
   - 6.6 [快速重传机制](#66-快速重传机制)
   - 6.7 [拥塞控制](#67-拥塞控制)
   - 6.8 [窗口探测](#68-窗口探测)
   - 6.9 [flush 核心函数](#69-flush-核心函数)
   - 6.10 [update 调度](#610-update-调度)
7. [协议配置与调优](#7-协议配置与调优)
   - 7.1 [工作模式](#71-工作模式)
   - 7.2 [窗口大小](#72-窗口大小)
   - 7.3 [MTU 设置](#73-mtu-设置)
   - 7.4 [最小 RTO](#74-最小-rto)
   - 7.5 [流模式 vs 消息模式](#75-流模式-vs-消息模式)
   - 7.6 [配置推荐](#76-配置推荐)
8. [API 参考](#8-api-参考)
9. [使用示例](#9-使用示例)
   - 9.1 [最小可运行示例](#91-最小可运行示例)
   - 9.2 [实际项目集成](#92-实际项目集成)
10. [协议比较](#10-协议比较)
11. [最佳实践](#11-最佳实践)
12. [生态与案例](#12-生态与案例)
13. [常见问题](#13-常见问题)
14. [附录：源码常量定义](#14-附录源码常量定义)

---

## 1. 概述

KCP 是一个**快速可靠协议**（Fast and Reliable ARQ Protocol），能以比 TCP 浪费 10%-20% 的带宽为代价，换取：

- **平均延迟降低 30%-40%**
- **最大延迟降低三倍**

核心设计理念：

| 特性 | 说明 |
|------|------|
| 纯算法实现 | 不负责底层协议（如 UDP）的收发 |
| 零系统调用 | 连时钟都需外部传入，内部无任何系统调用 |
| 极致轻量 | 仅 `ikcp.h` + `ikcp.c` 两个文件，约 1300 行 C 代码 |
| 回调驱动 | 通过 callback 让用户自定义底层发送方式 |
| 可移植 | 无第三方依赖，纯 C 实现，跨平台 |

> **形象比喻**：TCP 是一条流速很慢但每秒流量很大的**大运河**，KCP 则是水流湍急的**小激流**。TCP 追求的是"每秒传输多少 KB"（流量），KCP 追求的是"一个数据包从一端到另一端需要多少时间"（流速）。

---

## 2. 背景知识：为什么需要 KCP

### 2.1 TCP 的设计目标

TCP 为**流量**而设计，核心目标是**充分利用带宽**。它通过以下机制保障这一点：

- 拥塞避免与慢启动
- 延迟 ACK（通常 200ms）
- 超时重传时 RTO 指数退避

这些机制在**吞吐量优先**的场景（文件下载、视频流）中表现优秀，但在**延迟敏感**的场景（游戏、实时通信）中会带来严重问题。

### 2.2 实时场景的痛点

以网络游戏为例：

```
玩家 A 按下攻击键 → 数据包发出 → 服务器收到 → 广播给玩家 B
```

理想情况下，这个过程应在 50-100ms 内完成。但在公网环境下：

- 公网高峰期平均丢包率接近 10%，WiFi/3G/4G 更高
- TCP 一次丢包可能导致 RTO 翻倍（从 200ms → 400ms → 800ms）
- 延迟 ACK 导致 RTT 估算偏大
- 拥塞控制在丢包时激进降速

结果是：**一次丢包就可能造成数百毫秒甚至数秒的卡顿**。

### 2.3 UDP 的不足

UDP 虽然延迟低，但**不保证可靠性**——丢包、乱序、重复都无法处理。对于大多数应用来说，直接使用 UDP 是不够的。

### 2.4 KCP 的定位

KCP 在 UDP 之上实现了**可靠传输**，同时通过一系列优化策略大幅降低延迟。它不是 TCP 的替代品，而是**为延迟敏感场景量身定制的可靠传输方案**。

---

## 3. 核心原理

### 3.1 ARQ 协议基础

ARQ（Automatic Repeat reQuest，自动重传请求）是可靠传输的基础。其核心思想是：

1. 发送方为每个数据包编号
2. 接收方收到后返回确认（ACK）
3. 发送方超时未收到 ACK 则重传

KCP 属于 **选择性重传 ARQ**（Selective Repeat ARQ），是 ARQ 协议中效率最高的变体。

### 3.2 TCP 的延迟瓶颈

TCP 的延迟主要来自以下机制：

```
                    丢包发生
                       │
         ┌─────────────┼─────────────┐
         │             │             │
    RTO 指数退避   全部重传     延迟 ACK
   (200→400→800ms) (Go-Back-N)   (200ms)
         │             │             │
         └─────────────┼─────────────┘
                       │
               延迟急剧上升
```

### 3.3 KCP 的六大优化策略

#### 策略一：RTO 不翻倍（退避更温和）

| | TCP | KCP 快速模式 |
|---|---|---|
| 首次超时 | RTO × 2 | RTO × 1.5 |
| 连续丢 3 次包 | RTO × 8 | RTO × 3.375 |

TCP 的 RTO 每次超时翻倍，连续丢包会快速膨胀。KCP 快速模式采用 ×1.5 的温和增长（实验证明 1.5 是较优值），超时恢复更快。

**源码实现**（`ikcp_flush` 中）：

```c
if (kcp->nodelay == 0) {
    // 正常模式：RTO += max(当前RTO, 基础RTO)
    segment->rto += _imax_(segment->rto, (IUINT32)kcp->rx_rto);
} else {
    // 快速模式：RTO += rx_rto / 2
    IINT32 step = (kcp->nodelay < 2) ? 
        ((IINT32)(segment->rto)) : kcp->rx_rto;
    segment->rto += step / 2;
}
```

#### 策略二：选择性重传

**TCP** 使用 Go-Back-N：丢包时从丢失的包开始**全部重传**后续所有数据。

**KCP** 使用选择性重传：**只重传真正丢失的数据包**。

示例：发送 1,2,3,4,5，其中 2 丢失

```
TCP:  重传 2,3,4,5（4个包）
KCP:  重传 2（1个包）
```

**源码实现**（`ikcp_parse_ack`）：收到 ACK 后，只从 `snd_buf` 中删除对应序号的段，其余段独立管理重传。

#### 策略三：快速重传

不需要等超时，通过 ACK 跳越次数判断丢包：

```
发送: 1, 2, 3, 4, 5
收到: ACK1, ACK3, ACK4, ACK5

ACK3 → 2被跳过1次
ACK4 → 2被跳过2次 → 触发快速重传！
```

**源码实现**（`ikcp_parse_fastack`）：

```c
// 当收到一个 ACK 的 sn 大于某个段的 sn 时，
// 该段的 fastack 计数器加 1
if (sn != seg->sn) {
    seg->fastack++;
}
```

在 `ikcp_flush` 中判断：

```c
else if (segment->fastack >= resent) {  // resent 通常为 2
    if ((int)segment->xmit <= kcp->fastlimit || kcp->fastlimit <= 0) {
        needsend = 1;  // 触发快速重传
        segment->xmit++;
        segment->fastack = 0;
        change++;
    }
}
```

#### 策略四：非延迟 ACK

**TCP** 即使设置了 `TCP_NODELAY`，ACK 发送仍有延迟，导致 RTT 估算偏大。

**KCP** 的 ACK 是否延迟可以由用户配置。在快速模式下，ACK 立即发送，使 RTT 估算更准确，丢包判断更及时。

#### 策略五：ACK + UNA 双重确认

| 确认方式 | 含义 | 优缺点 |
|----------|------|--------|
| UNA | 此编号前所有包已收到 | 简单，但丢包导致全部重传 |
| ACK | 该编号包已收到 | 精确，但丢失成本高 |

**KCP 的方案**：所有数据包都携带 UNA 信息（`una` 字段），同时单独发送 ACK 包。二者兼顾：

- UNA 信息随每个数据包捎带，不额外占用带宽
- ACK 精确确认每个包的接收情况
- 即使 ACK 丢失，后续包的 UNA 也能推进发送窗口

#### 策略六：非退让流控

KCP 提供两种流控模式：

| 模式 | 发送频率控制 | 适用场景 |
|------|-------------|---------|
| 正常模式 | 发送缓存 + 接收窗口 + 丢包退让 + 慢启动 | 公平性要求高 |
| 快速模式 | 仅发送缓存 + 接收窗口 | 延迟敏感，允许牺牲公平性 |

快速模式关闭了丢包退让和慢启动，即使网络拥塞也不会主动降速，牺牲公平性换取低延迟。

---

## 4. 协议报文格式

KCP 只有一种报文格式，数据和控制消息共享相同的头部：

```
 0               4   5   6       8 (BYTE)
+---------------+---+---+-------+
|     conv      |cmd|frg|  wnd  |
+---------------+---+---+-------+   8
|     ts        |     sn        |
+---------------+---------------+  16
|     una       |     len       |
+---------------+---------------+  24
|                               |
|        DATA (optional)        |
|                               |
+-------------------------------+
```

| 字段 | 长度 | 说明 |
|------|------|------|
| `conv` | 4 字节 | 会话 ID，通信双方必须相同才能识别彼此的数据包 |
| `cmd` | 1 字节 | 命令类型：81=PUSH, 82=ACK, 83=WASK, 84=WINS |
| `frg` | 1 字节 | 分片序号，消息模式：倒数第几个分片（0=最后一个）；流模式：始终为 0 |
| `wnd` | 2 字节 | 发送方当前可用接收窗口大小 |
| `ts` | 4 字节 | 时间戳（毫秒） |
| `sn` | 4 字节 | 序列号（数据包）或确认号（ACK 包） |
| `una` | 4 字节 | UNA：此编号前的所有包已收到 |
| `len` | 4 字节 | 数据长度 |
| DATA | 变长 | 实际数据 |

**报文头固定 24 字节**（`IKCP_OVERHEAD = 24`），最大数据载荷 = MTU - 24。

### 四种命令类型

| cmd 值 | 常量 | 名称 | 说明 |
|--------|------|------|------|
| 81 | `IKCP_CMD_PUSH` | 数据推送 | 携带应用数据 |
| 82 | `IKCP_CMD_ACK` | 确认 | 确认已收到某个数据包 |
| 83 | `IKCP_CMD_WASK` | 窗口探测请求 | 询问对方可用窗口大小 |
| 84 | `IKCP_CMD_WINS` | 窗口探测回复 | 告知对方自己的窗口大小 |

### conv 会话 ID 的设计

`conv` 是 32 位整数，用于标识连接。实践建议：

- 高 16 位：调用方索引
- 低 16 位：被调用方索引

这样双方可以通过 conv 中的索引快速定位对应的 KCP 对象。

---

## 5. 架构设计

### 5.1 整体架构

KCP 的架构哲学是**"拆开协议栈"**，让各个协议单元像积木一样自由组合：

```
┌─────────────────────────────────────┐
│           应用层                     │
├─────────────────────────────────────┤
│   非对称密钥交换（握手层）           │  ← 可选
├─────────────────────────────────────┤
│   类 RC4/Salsa20 流加密             │  ← 可选
├─────────────────────────────────────┤
│        ★ KCP 可靠传输 ★            │  ← 核心
├─────────────────────────────────────┤
│   Reed-Solomon 纠删码（FEC）        │  ← 可选
├─────────────────────────────────────┤
│   动态路由系统（多路径探测选优）     │  ← 可选
├─────────────────────────────────────┤
│        UDP 传输层                   │  ← 用户提供
└─────────────────────────────────────┘
```

KCP 只负责中间的"可靠传输"层，其他层由用户根据需要自行组装。

### 5.2 核心数据结构

#### IKCPSEG - 报文段

```c
struct IKCPSEG {
    struct IQUEUEHEAD node;   // 双向链表节点（用于队列管理）
    IUINT32 conv;             // 会话 ID
    IUINT32 cmd;              // 命令类型
    IUINT32 frg;              // 分片计数
    IUINT32 wnd;              // 窗口大小
    IUINT32 ts;               // 时间戳
    IUINT32 sn;               // 序列号
    IUINT32 una;              // UNA 确认号
    IUINT32 len;              // 数据长度
    IUINT32 resendts;         // 重发时间戳
    IUINT32 rto;              // 重传超时时间
    IUINT32 fastack;          // 快速重传计数器（被跳过次数）
    IUINT32 xmit;             // 发送次数
    char data[1];             // 数据（柔性数组）
};
```

#### IKCPCB - KCP 控制块

```c
struct IKCPCB {
    IUINT32 conv, mtu, mss, state;
    IUINT32 snd_una, snd_nxt, rcv_nxt;
    IUINT32 ts_recent, ts_lastack, ssthresh;
    IINT32 rx_rttval, rx_srtt, rx_rto, rx_minrto;
    IUINT32 snd_wnd, rcv_wnd, rmt_wnd, cwnd, probe;
    IUINT32 current, interval, ts_flush, xmit;
    IUINT32 nrcv_buf, nsnd_buf;
    IUINT32 nrcv_que, nsnd_que;
    IUINT32 nodelay, updated;
    IUINT32 ts_probe, probe_wait;
    IUINT32 dead_link, incr;
    struct IQUEUEHEAD snd_queue;    // 发送队列
    struct IQUEUEHEAD rcv_queue;    // 接收队列
    struct IQUEUEHEAD snd_buf;      // 发送缓冲
    struct IQUEUEHEAD rcv_buf;      // 接收缓冲
    IUINT32 *acklist;               // 待发送的 ACK 列表
    IUINT32 ackcount;               // ACK 计数
    IUINT32 ackblock;               // ACK 列表容量
    void *user;                     // 用户数据指针
    char *buffer;                   // 输出缓冲区
    int fastresend;                 // 快速重传触发阈值
    int fastlimit;                  // 快速重传次数上限
    int nocwnd, stream;             // 是否关闭拥塞控制、是否流模式
    int logmask;                    // 日志掩码
    int (*output)(const char *buf, int len, struct IKCPCB *kcp, void *user);
    void (*writelog)(const char *log, struct IKCPCB *kcp, void *user);
};
```

**关键字段说明**：

| 字段 | 含义 |
|------|------|
| `snd_una` | 第一个未确认的发送序号 |
| `snd_nxt` | 下一个待分配的发送序号 |
| `rcv_nxt` | 下一个期望接收的序号 |
| `cwnd` | 拥塞窗口大小 |
| `ssthresh` | 慢启动阈值 |
| `rx_srtt` | 平滑 RTT |
| `rx_rttval` | RTT 方差 |
| `rx_rto` | 当前重传超时时间 |
| `rmt_wnd` | 远端接收窗口大小 |

### 5.3 四大队列

KCP 维护四个双向链表队列，是理解 KCP 数据流转的关键：

```
发送侧：
  snd_queue（发送队列）  ──→  snd_buf（发送缓冲）
  [等待发送的数据]          [已发送但未确认的数据]

接收侧：
  rcv_buf（接收缓冲）   ──→  rcv_queue（接收队列）
  [收到的乱序/待排序数据]   [已排序、可被应用读取的数据]
```

| 队列 | 存储内容 | 操作 |
|------|---------|------|
| `snd_queue` | 等待发送的数据段 | `ikcp_send` 入队，`ikcp_flush` 移至 `snd_buf` |
| `snd_buf` | 已发送未确认的数据段 | 收到 ACK 后移除，超时后重传 |
| `rcv_buf` | 收到的但还不连续的数据段 | 收到数据时入队，连续后移至 `rcv_queue` |
| `rcv_queue` | 连续的、可被应用读取的数据段 | `ikcp_recv` 从此队列读取 |

### 5.4 数据流转

#### 发送流程

```
应用数据 → ikcp_send()
              │
              ├─ 分片（超过 MSS 时）
              │
              ▼
         snd_queue
              │
              ├─ ikcp_flush() 移至 snd_buf
              │  （受 cwnd 和窗口限制）
              │
              ▼
         snd_buf
              │
              ├─ 首次发送：xmit=0 → 立即发送
              ├─ 超时重传：current >= resendts → 重传
              ├─ 快速重传：fastack >= fastresend → 重传
              │
              ▼
        output 回调 → UDP 发送
              │
              ├─ 收到 ACK → 从 snd_buf 移除
              └─ 收到 UNA → 移除 una 之前的所有段
```

#### 接收流程

```
UDP 收包 → ikcp_input()
              │
              ├─ 解析报文头
              ├─ 校验 conv
              │
              ├─ CMD=ACK → 更新 RTT，从 snd_buf 移除
              ├─ CMD=PUSH → 加入 ACK 列表，数据入 rcv_buf
              ├─ CMD=WASK → 标记需要回复窗口
              ├─ CMD=WINS → 更新远端窗口
              │
              ▼
         rcv_buf（按 sn 排序，去重）
              │
              ├─ 检查连续性：seg->sn == rcv_nxt
              │  且 rcv_que < rcv_wnd
              │
              ▼
         rcv_queue → ikcp_recv() → 应用数据
```

---

## 6. 实现详解

### 6.1 创建与销毁

#### ikcp_create

```c
ikcpcb* ikcp_create(IUINT32 conv, void *user);
```

创建 KCP 控制块，初始化所有状态：

- `conv`：会话标识，双方必须一致
- `user`：传递给 output 回调的用户指针
- 默认 MTU = 1400，MSS = 1400 - 24 = 1376
- 默认发送窗口 = 32，接收窗口 = 128
- 默认 RTO = 200ms，最小 RTO = 100ms
- 默认 interval = 100ms

#### ikcp_release

```c
void ikcp_release(ikcpcb *kcp);
```

释放 KCP 控制块，清理四个队列中所有数据段、ACK 列表和输出缓冲区。

### 6.2 数据发送流程

#### ikcp_send

```c
int ikcp_send(ikcpcb *kcp, const char *buffer, int len);
```

**关键步骤**：

1. **流模式合并**：如果 `stream != 0`，尝试将数据追加到 `snd_queue` 尾部段的空余空间
2. **分片**：按 MSS 大小将数据分成多个段
   - 每段 `frg` 字段 = `count - i - 1`（消息模式）或 `0`（流模式）
   - `frg == 0` 表示是最后一个分片
3. **入队**：所有段加入 `snd_queue`

**分片数量限制**：不能超过 `IKCP_WND_RCV`（128），因为接收端窗口有限，过大的消息无法完整接收。

### 6.3 数据接收流程

#### ikcp_recv

```c
int ikcp_recv(ikcpcb *kcp, char *buffer, int len);
```

**关键步骤**：

1. 检查 `rcv_queue` 是否为空
2. 用 `ikcp_peeksize` 计算下一个完整消息的大小
3. 合并分片：从 `rcv_queue` 头部取出段，直到 `frg == 0`
4. 将数据拷贝到用户缓冲区
5. 将 `rcv_buf` 中连续的数据段移至 `rcv_queue`
6. 如果之前接收窗口满，现在有空位，设置 `probe |= IKCP_ASK_TELL` 通知远端

**返回值**：
- 正数：接收到的数据字节数
- -1：没有可读数据
- -2：分片不完整
- -3：用户缓冲区太小

#### ikcp_peeksize

```c
int ikcp_peeksize(const ikcpcb *kcp);
```

预览下一个完整消息的大小（包括所有分片），用于提前分配缓冲区。

### 6.4 ACK 处理机制

KCP 的 ACK 处理分为三步：

#### 1. 发送 ACK

收到 PUSH 数据时，调用 `ikcp_ack_push` 将 `(sn, ts)` 加入待发送 ACK 列表。在下次 `ikcp_flush` 时批量发送。

#### 2. 接收 ACK

`ikcp_input` 中处理 `CMD=ACK` 的报文：

```c
if (cmd == IKCP_CMD_ACK) {
    // 1. 更新 RTT 估算
    if (_itimediff(kcp->current, ts) >= 0) {
        ikcp_update_ack(kcp, _itimediff(kcp->current, ts));
    }
    // 2. 从 snd_buf 中移除对应段
    ikcp_parse_ack(kcp, sn);
    // 3. 更新 snd_una
    ikcp_shrink_buf(kcp);
    // 4. 记录最大 ACK 用于快速重传判断
}
```

#### 3. 处理 UNA

每个报文都携带 `una` 字段，表示"此编号前的所有包已收到"：

```c
static void ikcp_parse_una(ikcpcb *kcp, IUINT32 una) {
    // 从 snd_buf 头部开始，删除所有 sn < una 的段
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        if (_itimediff(una, seg->sn) > 0) {
            iqueue_del(p);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
        } else {
            break;
        }
    }
}
```

### 6.5 RTO 计算与超时重传

#### RTT 估算（Jacobson/Karels 算法）

```c
static void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt) {
    if (kcp->rx_srtt == 0) {
        // 首次测量
        kcp->rx_srtt = rtt;
        kcp->rx_rttval = rtt / 2;
    } else {
        // 后续测量
        long delta = rtt - kcp->rx_srtt;
        if (delta < 0) delta = -delta;
        kcp->rx_rttval = (3 * kcp->rx_rttval + delta) / 4;   // RTT 方差
        kcp->rx_srtt = (7 * kcp->rx_srtt + rtt) / 8;          // 平滑 RTT
        if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
    }
    // RTO = SRTT + max(interval, 4 * RTTVAL)
    rto = kcp->rx_srtt + _imax_(kcp->interval, 4 * kcp->rx_rttval);
    // 限制 RTO 在 [rx_minrto, IKCP_RTO_MAX] 范围内
    kcp->rx_rto = _ibound_(kcp->rx_minrto, rto, IKCP_RTO_MAX);
}
```

#### 超时重传

在 `ikcp_flush` 中：

```c
else if (_itimediff(current, segment->resendts) >= 0) {
    needsend = 1;
    segment->xmit++;
    kcp->xmit++;
    // RTO 退避
    if (kcp->nodelay == 0) {
        segment->rto += _imax_(segment->rto, (IUINT32)kcp->rx_rto);
    } else {
        IINT32 step = (kcp->nodelay < 2) ? 
            ((IINT32)(segment->rto)) : kcp->rx_rto;
        segment->rto += step / 2;  // 温和退避
    }
    segment->resendts = current + segment->rto;
    lost = 1;  // 标记丢包事件
}
```

### 6.6 快速重传机制

快速重传不需要等待超时，通过 ACK 跳越次数触发：

1. `ikcp_input` 中处理 ACK 时，调用 `ikcp_parse_fastack`
2. 遍历 `snd_buf`，所有 `sn < maxack` 的段，`fastack++`
3. `ikcp_flush` 中判断：`segment->fastack >= fastresend` 时触发重传
4. 默认 `fastresend = 0`（关闭），推荐设置为 2

```c
// ikcp_flush 中的快速重传判断
else if (segment->fastack >= resent) {
    if ((int)segment->xmit <= kcp->fastlimit || kcp->fastlimit <= 0) {
        needsend = 1;
        segment->xmit++;
        segment->fastack = 0;
        segment->resendts = current + segment->rto;
        change++;  // 标记快速重传事件
    }
}
```

### 6.7 拥塞控制

KCP 的拥塞控制与 TCP 类似，但可以关闭：

#### 慢启动 → 拥塞避免

```c
// 在 ikcp_input 中，当 snd_una 推进时
if (kcp->cwnd < kcp->ssthresh) {
    kcp->cwnd++;                    // 慢启动：每收到一个 ACK，cwnd + 1
    kcp->incr += mss;
} else {
    // 拥塞避免：线性增长
    if (kcp->incr < mss) kcp->incr = mss;
    kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
    if ((kcp->cwnd + 1) * mss <= kcp->incr) {
        kcp->cwnd = (kcp->incr + mss - 1) / ((mss > 0)? mss : 1);
    }
}
```

#### 快速重传时的拥塞调整

```c
// ikcp_flush 中
if (change) {
    // 快速重传触发：ssthresh 降为 in-flight 的一半
    IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
    kcp->ssthresh = inflight / 2;
    if (kcp->ssthresh < IKCP_THRESH_MIN) kcp->ssthresh = IKCP_THRESH_MIN;
    kcp->cwnd = kcp->ssthresh + resent;
    kcp->incr = kcp->cwnd * kcp->mss;
}

if (lost) {
    // 超时丢包：ssthresh 降为 cwnd 的一半，cwnd 降为 1
    kcp->ssthresh = cwnd / 2;
    if (kcp->ssthresh < IKCP_THRESH_MIN) kcp->ssthresh = IKCP_THRESH_MIN;
    kcp->cwnd = 1;
    kcp->incr = kcp->mss;
}
```

当 `nocwnd = 1` 时，拥塞控制被关闭，发送窗口仅受 `snd_wnd` 和 `rmt_wnd` 限制。

### 6.8 窗口探测

当远端接收窗口为 0 时（`rmt_wnd == 0`），KCP 需要定期探测对方窗口是否恢复：

```c
if (kcp->rmt_wnd == 0) {
    if (kcp->probe_wait == 0) {
        kcp->probe_wait = IKCP_PROBE_INIT;  // 初始 7000ms
        kcp->ts_probe = kcp->current + kcp->probe_wait;
    } else {
        if (_itimediff(kcp->current, kcp->ts_probe) >= 0) {
            // 指数退避探测间隔，上限 120 秒
            kcp->probe_wait += kcp->probe_wait / 2;
            if (kcp->probe_wait > IKCP_PROBE_LIMIT)
                kcp->probe_wait = IKCP_PROBE_LIMIT;
            kcp->ts_probe = kcp->current + kcp->probe_wait;
            kcp->probe |= IKCP_ASK_SEND;  // 标记需要发送 WASK
        }
    }
}
```

### 6.9 flush 核心函数

`ikcp_flush` 是 KCP 的核心函数，在每次 `ikcp_update` 到期时被调用，负责：

1. **发送 ACK**：批量发送待确认的 ACK 列表
2. **窗口探测**：发送 WASK/WINS 命令
3. **数据发送**：将 `snd_queue` 中的数据移至 `snd_buf` 并发送
4. **重传处理**：处理超时重传和快速重传
5. **拥塞调整**：根据重传事件调整拥塞窗口

数据发送时会将多个小包合并到一个 UDP 包中（不超过 MTU），减少系统调用次数。

### 6.10 update 调度

```c
void ikcp_update(ikcpcb *kcp, IUINT32 current) {
    kcp->current = current;
    if (kcp->updated == 0) {
        kcp->updated = 1;
        kcp->ts_flush = kcp->current;
    }
    slap = _itimediff(kcp->current, kcp->ts_flush);
    if (slap >= 10000 || slap < -10000) {
        kcp->ts_flush = kcp->current;
        slap = 0;
    }
    if (slap >= 0) {
        kcp->ts_flush += kcp->interval;
        if (_itimediff(kcp->current, kcp->ts_flush) >= 0) {
            kcp->ts_flush = kcp->current + kcp->interval;
        }
        ikcp_flush(kcp);
    }
}
```

`ikcp_check` 用于优化大规模连接时的调度效率：

```c
IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current);
```

返回下次应该调用 `ikcp_update` 的时间戳，避免不必要的空转。在管理数千个 KCP 连接时，可以用 `ikcp_check` 实现类似 epoll 的高效调度。

---

## 7. 协议配置与调优

### 7.1 工作模式

```c
int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc);
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `nodelay` | 是否启用 nodelay 模式 | 0 |
| `interval` | 内部更新间隔（ms） | 100 |
| `resend` | 快速重传阈值（0=关闭） | 0 |
| `nc` | 是否关闭拥塞控制 | 0 |

**nodelay 参数影响**：
- `nodelay = 0`：`rx_minrto = 100ms`
- `nodelay = 1`：`rx_minrto = 30ms`

**resend 参数**：收到 `resend` 个跳越 ACK 后立即重传。推荐值为 2。

**nc 参数**：
- `nc = 0`：正常拥塞控制（慢启动 + 拥塞避免 + 丢包退让）
- `nc = 1`：关闭拥塞控制，仅受窗口限制

### 7.2 窗口大小

```c
int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd);
```

- 默认发送窗口 = 32 包
- 默认接收窗口 = 128 包（**注意**：`rcvwnd` 不能小于 128，因为消息模式下单条消息的分片数受此限制）
- 单位是**包**而非字节

窗口大小设置建议：

```
窗口大小 ≈ 平均延迟(ms) × 发送频率(包/ms) × 冗余系数(1.5~2)
```

例如：平均延迟 200ms，每 20ms 发一个包，冗余系数 2：
```
窗口 = 200 / 20 × 2 = 20，考虑丢包重传，设为 128
```

### 7.3 MTU 设置

```c
int ikcp_setmtu(ikcpcb *kcp, int mtu);
```

- 默认 MTU = 1400 字节
- MSS = MTU - 24（报文头大小）
- KCP 不负责 MTU 探测，需用户自行处理
- 建议值：
  - 以太网：1400（默认）
  - 互联网：1200（更保守）
  - 避免超过路径 MTU 导致 IP 分片

### 7.4 最小 RTO

```c
kcp->rx_minrto = 10;  // 手动设置，单位 ms
```

| 模式 | 默认值 |
|------|--------|
| 正常模式 | 100ms |
| 快速模式 | 30ms |

降低 `rx_minrto` 可以更快检测丢包，但可能增加误判（将正常延迟误认为丢包）。

### 7.5 流模式 vs 消息模式

```c
kcp->stream = 1;  // 启用流模式
```

| 模式 | 分片处理 | 数据边界 | 适用场景 |
|------|---------|---------|---------|
| 消息模式（默认） | 每次发送独立编号，`frg` 标记分片 | 保留 | 游戏指令、RPC |
| 流模式 | 新数据追加到上一段（如果还有空间），`frg = 0` | 不保留 | 文件传输、流媒体 |

### 7.6 配置推荐

#### 游戏实时同步（极速模式）

```c
ikcp_nodelay(kcp, 1, 10, 2, 1);  // nodelay=1, interval=10ms, resend=2, nc=1
ikcp_wndsize(kcp, 256, 256);
kcp->rx_minrto = 10;
```

#### 视频推流

```c
ikcp_nodelay(kcp, 1, 20, 2, 1);
ikcp_wndsize(kcp, 512, 512);
```

#### 文件传输（公平模式）

```c
ikcp_nodelay(kcp, 0, 40, 0, 0);  // 类似 TCP 行为
ikcp_wndsize(kcp, 256, 256);
```

#### 一般应用

```c
ikcp_nodelay(kcp, 1, 10, 2, 0);  // 开启 nodelay 但保留拥塞控制
```

---

## 8. API 参考

### 核心接口

| 函数 | 说明 |
|------|------|
| `ikcp_create(conv, user)` | 创建 KCP 控制块 |
| `ikcp_release(kcp)` | 释放 KCP 控制块 |
| `ikcp_send(kcp, buf, len)` | 发送数据，返回发送字节数或错误码 |
| `ikcp_recv(kcp, buf, len)` | 接收数据，返回接收字节数或错误码 |
| `ikcp_input(kcp, data, size)` | 输入底层收到的数据包 |
| `ikcp_update(kcp, current)` | 更新状态（定时调用，传入毫秒时钟） |
| `ikcp_check(kcp, current)` | 查询下次 update 的时间 |

### 配置接口

| 函数 | 说明 |
|------|------|
| `ikcp_nodelay(kcp, nodelay, interval, resend, nc)` | 设置工作模式 |
| `ikcp_wndsize(kcp, sndwnd, rcvwnd)` | 设置窗口大小 |
| `ikcp_setmtu(kcp, mtu)` | 设置 MTU |

### 辅助接口

| 函数 | 说明 |
|------|------|
| `ikcp_peeksize(kcp)` | 预览下一个消息大小 |
| `ikcp_waitsnd(kcp)` | 待发送包数（`nsnd_buf + nsnd_que`） |
| `ikcp_setoutput(kcp, output)` | 设置输出回调 |
| `ikcp_getconv(ptr)` | 从原始数据中读取 conv |
| `ikcp_flush(kcp)` | 立即刷新（通常不需要手动调用） |
| `ikcp_allocator(malloc, free)` | 自定义内存分配器 |

### 错误码

| 返回值 | 含义 |
|--------|------|
| 正数 | 成功，数据字节数 |
| -1 | 无数据 / 参数错误 |
| -2 | 分片不完整 / 内存不足 / 分片过多 |
| -3 | 用户缓冲区太小 / 未知命令 |

### 死链检测

```c
kcp->dead_link = 20;  // 默认值
```

当某个段的发送次数 `xmit >= dead_link` 时，`kcp->state` 被设为 `-1`，表示连接已断开。

---

## 9. 使用示例

### 9.1 最小可运行示例

以下示例展示了 KCP 的基本使用流程：

```c
#include "ikcp.h"
#include <stdio.h>
#include <string.h>

// UDP 发送回调
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    // 在这里将 buf/len 通过 UDP 发送给远端
    // udp_send_to(remote_addr, buf, len);
    return 0;
}

int main() {
    // 1. 创建 KCP 对象
    IUINT32 conv = 0x11223344;
    ikcpcb *kcp = ikcp_create(conv, NULL);
    
    // 2. 设置输出回调
    kcp->output = udp_output;
    
    // 3. 配置为极速模式
    ikcp_nodelay(kcp, 1, 10, 2, 1);
    ikcp_wndsize(kcp, 128, 128);
    
    // 4. 主循环
    while (1) {
        // 4a. 获取当前时间（毫秒）
        IUINT32 current = get_current_millisec();
        
        // 4b. 更新 KCP 状态
        ikcp_update(kcp, current);
        
        // 4c. 收到 UDP 包时输入到 KCP
        // char udp_buf[2000];
        // int udp_len = udp_recv_from(udp_buf, sizeof(udp_buf));
        // if (udp_len > 0) {
        //     ikcp_input(kcp, udp_buf, udp_len);
        // }
        
        // 4d. 发送数据
        // ikcp_send(kcp, data, data_len);
        
        // 4e. 接收数据
        // char kcp_buf[2000];
        // int kcp_len = ikcp_recv(kcp, kcp_buf, sizeof(kcp_buf));
        // if (kcp_len > 0) {
        //     // 处理接收到的数据
        // }
        
        sleep_ms(1);
    }
    
    // 5. 释放
    ikcp_release(kcp);
    return 0;
}
```

### 9.2 实际项目集成

以下展示了一个更完整的集成模式，包含连接管理和多路复用：

```c
#include "ikcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>

// 连接管理器
class KcpManager {
    struct Connection {
        ikcpcb *kcp;
        int fd;  // 关联的 UDP socket
        struct sockaddr_in addr;
    };
    
    std::map<IUINT32, Connection> connections;
    int udp_fd;
    
public:
    // UDP 输出回调
    static int udp_output(const char *buf, int len, ikcpcb *kcp, void *user) {
        KcpManager *mgr = (KcpManager*)user;
        IUINT32 conv = kcp->conv;
        // 查找连接信息，发送 UDP 包
        auto it = mgr->connections.find(conv);
        if (it != mgr->connections.end()) {
            sendto(it->second.fd, buf, len, 0,
                   (struct sockaddr*)&it->second.addr,
                   sizeof(it->second.addr));
        }
        return 0;
    }
    
    // 创建新连接
    ikcpcb* create_connection(IUINT32 conv, const struct sockaddr_in &addr) {
        ikcpcb *kcp = ikcp_create(conv, this);
        kcp->output = udp_output;
        
        // 极速模式配置
        ikcp_nodelay(kcp, 1, 10, 2, 1);
        ikcp_wndsize(kcp, 256, 256);
        kcp->rx_minrto = 10;
        
        Connection conn = {kcp, udp_fd, addr};
        connections[conv] = conn;
        return kcp;
    }
    
    // 处理收到的 UDP 数据
    void on_udp_packet(const char *data, int len, const struct sockaddr_in &addr) {
        // 从数据中提取 conv
        IUINT32 conv = ikcp_getconv(data);
        
        auto it = connections.find(conv);
        if (it == connections.end()) {
            // 新连接，创建 KCP 对象
            create_connection(conv, addr);
            it = connections.find(conv);
        }
        
        // 输入到 KCP
        ikcp_input(it->second.kcp, data, len);
    }
    
    // 定时更新所有连接
    void update() {
        IUINT32 current = get_current_millisec();
        for (auto &pair : connections) {
            ikcp_update(pair.second.kcp, current);
        }
    }
    
    // 检查死链
    void check_dead_links() {
        for (auto it = connections.begin(); it != connections.end(); ) {
            if (it->second.kcp->state == (IUINT32)-1) {
                ikcp_release(it->second.kcp);
                it = connections.erase(it);
            } else {
                ++it;
            }
        }
    }
};
```

---

## 10. 协议比较

### KCP vs TCP vs enet vs UDT

| 特性 | KCP | TCP | enet | UDT |
|------|-----|-----|------|-----|
| **传输层** | UDP | IP | UDP | UDP |
| **可靠性** | 是 | 是 | 可选 | 是 |
| **平均延迟** | 低 | 高 | 中 | 极高 |
| **丢包恢复** | 快 | 慢 | 中 | 极慢 |
| **带宽开销** | 10-20% | 0% | ~10% | 较高 |
| **公平性** | 可配置 | 好 | 一般 | 好 |
| **代码量** | ~1300行 | - | ~15000行 | ~20000行 |
| **集成难度** | 低 | - | 中 | 中 |

### asio-kcp 作者的评测结论

- KCP 在 WiFi 和手机网络（3G, 4G）下表现良好
- **KCP 是实时 PvP 游戏的首选**
- 网络延迟发生时卡顿小于 1 秒，比 enet 好 3 倍
- enet 是允许 2 秒延迟的游戏的好选择
- **UDT 是糟糕的选择**，经常陷入数秒延迟

### SpatialOS 评测

SpatialOS 在服务端刷新率 60Hz 同时维护 50 个角色时，KCP 对比 TCP/RakNet 有显著响应时间优势。

---

## 11. 最佳实践

### 11.1 update 调用频率

- 推荐每 10ms 调用一次 `ikcp_update`
- 大规模连接（3500+）时使用 `ikcp_check` 优化调度
- 切勿使用大于 100ms 的间隔

### 11.2 与 TCP 服务器共存

KCP 运行在 UDP 之上，可以与 TCP 服务器共享端口：

- 服务器同时监听 TCP 和 UDP
- 客户端先通过 TCP 握手，协商 conv
- 后续数据通过 KCP/UDP 传输

### 11.3 数据加密

KCP 本身不提供加密，建议在 KCP 之上添加加密层：

```
应用数据 → 加密 → ikcp_send() → UDP
UDP → ikcp_recv() → 解密 → 应用数据
```

推荐使用 AES-128-GCM 或 ChaCha20-Poly1305。

### 11.4 应用层流量控制

KCP 的窗口控制是基于包数的，如果应用数据大小不一，建议在应用层实现流量控制：

- 监控 `ikcp_waitsnd(kcp)` 的值
- 当待发送包数超过阈值时，暂停应用层发送
- 等待待发送包数降低后恢复

### 11.5 自定义内存分配器

对于嵌入式系统或需要精细内存控制的场景：

```c
void* my_malloc(size_t size) { return pool_alloc(size); }
void my_free(void *ptr) { pool_free(ptr); }

ikcp_allocator(my_malloc, my_free);
```

**注意**：必须在创建任何 KCP 对象之前调用。

### 11.6 连接管理

KCP 不负责连接的建立和断开，需要用户自行实现：

- **握手**：交换 conv、初始窗口等信息
- **心跳**：定期发送空包检测死链
- **超时**：通过 `kcp->state == -1` 检测死链
- **conv 分配**：高 16 位为服务端索引，低 16 位为客户端索引

### 11.7 MTU 探测

KCP 不负责 MTU 探测。建议：

- 使用保守的 MTU（1200）
- 或自行实现 ICMP 黑洞探测
- 避免超过路径 MTU 导致 IP 分片

---

## 12. 生态与案例

### 多语言实现

| 语言 | 项目 | 特点 |
|------|------|------|
| Go | [kcp-go](https://github.com/xtaci/kcp-go) | 高安全性，包含会话管理 |
| Java | [kcp-netty](https://github.com/szhnet/kcp-netty) | 基于 Netty |
| C# | [KcpTransport](https://github.com/Cysharp/KcpTransport) | Syn Cookie 握手、连接管理 |
| C# | [kcp2k](https://github.com/vis2k/kcp2k/) | 逐行翻译，Unity 友好 |
| Rust | [kcp-rs](https://github.com/en/kcp-rs) | Rust 原生实现 |
| Python | [kcp.py](https://github.com/RealistikDash/kcp.py) | 开发者友好 |
| Lua | [lua-kcp](https://github.com/linxiaolong/lua-kcp) | Lua 扩展 |
| Node.js | [node-kcp](https://github.com/leenjewel/node-kcp) | Node.js 接口 |
| PHP | [php-ext-kcp](https://github.com/wpjscc/php-ext-kcp) | PHP 扩展 |

### 知名开源项目

| 项目 | 说明 |
|------|------|
| [kcptun](https://github.com/xtaci/kcptun) | 基于 KCP 的高速隧道 |
| [v2ray](https://www.v2ray.com) | 代理软件，集成 KCP 协议 |
| [frp](https://github.com/fatedier/frp) | 高性能内网穿透反向代理 |
| [HP-Socket](https://github.com/ldcsaa/HP-Socket) | 高性能网络通信框架 |

### 商业案例

| 产品 | 应用 |
|------|------|
| **原神**（米哈游） | 降低游戏消息传输耗时 |
| **SpatialOS** | 大型多人游戏服务端引擎 |
| **网易 CC/BOBO** | 加速视频推流 |
| **网易 UU 加速器** | 远程传输加速 |
| **阿里云 GRTN** | 音视频数据传输优化 |
| **西山居** | 游戏数据加速 |
| **云帆加速** | 文件传输和视频推流加速 |

---

## 13. 常见问题

### Q1：KCP 能完全替代 TCP 吗？

**不能**。KCP 适用于延迟敏感的小数据传输（游戏、实时通信），对于大文件传输等吞吐量优先的场景，TCP 更合适。KCP 比 TCP 多消耗 10%-20% 带宽。

### Q2：KCP 需要自己实现握手吗？

**是的**。KCP 只负责可靠传输，不负责连接建立/断开、加密、握手等。这些需要用户在应用层实现。

### Q3：多个 KCP 连接如何复用一个 UDP 端口？

通过 `conv` 区分不同连接。收到 UDP 包后，用 `ikcp_getconv` 提取 conv，查找对应的 KCP 对象，再调用 `ikcp_input`。

### Q4：`ikcp_update` 应该多久调用一次？

推荐 10ms 一次。如果资源有限，可以用 `ikcp_check` 计算下次调用时间，避免空转。

### Q5：如何检测连接是否断开？

检查 `kcp->state`，如果为 `(IUINT32)-1` 则连接已断。这通常是因为某个段的发送次数超过 `dead_link`（默认 20 次）。

### Q6：KCP 的接收窗口为什么默认 128 而不是 32？

因为消息模式下，单条消息的分片数不能超过接收窗口大小。如果接收窗口太小，大消息无法完整接收。128 是一个平衡值。

### Q7：KCP 支持多线程吗？

KCP 本身不是线程安全的。如果需要在多线程中使用，需要用户自行加锁，或将 KCP 操作集中在一个线程中。

### Q8：如何减少 KCP 的带宽开销？

- 适当增大 `interval`（如从 10ms 改为 20ms）
- 关闭快速重传（`resend = 0`）
- 保留拥塞控制（`nc = 0`）
- 增大 MTU 减少头部占比

### Q9：`ikcp_flush` 可以手动调用吗？

可以，但通常不需要。`ikcp_update` 会在合适时机自动调用 `ikcp_flush`。如果需要立即发送缓冲区中的数据（如低延迟场景），可以手动调用。

### Q10：KCP 的 `conv` 如何选择？

- 通信双方必须一致
- 建议高 16 位为服务端索引，低 16 位为客户端索引
- 可以是随机数，但需确保不会冲突

---

## 14. 附录：源码常量定义

```c
// RTO 相关
IKCP_RTO_NDL    = 30       // nodelay 模式最小 RTO（ms）
IKCP_RTO_MIN    = 100      // 正常模式最小 RTO（ms）
IKCP_RTO_DEF    = 200      // 默认 RTO（ms）
IKCP_RTO_MAX    = 60000    // 最大 RTO（ms）

// 命令类型
IKCP_CMD_PUSH   = 81       // 数据推送
IKCP_CMD_ACK    = 82       // 确认
IKCP_CMD_WASK   = 83       // 窗口探测请求
IKCP_CMD_WINS   = 84       // 窗口探测回复

// 窗口
IKCP_WND_SND    = 32       // 默认发送窗口
IKCP_WND_RCV    = 128      // 默认接收窗口

// 其他
IKCP_MTU_DEF    = 1400     // 默认 MTU
IKCP_ACK_FAST   = 3        // ACK 快速重传
IKCP_INTERVAL   = 100      // 默认更新间隔（ms）
IKCP_OVERHEAD   = 24       // 报文头大小（字节）
IKCP_DEADLINK   = 20       // 死链判定（重传次数）
IKCP_THRESH_INIT = 2       // 慢启动初始阈值
IKCP_THRESH_MIN  = 2       // 慢启动最小阈值
IKCP_PROBE_INIT  = 7000    // 窗口探测初始间隔（ms）
IKCP_PROBE_LIMIT = 120000  // 窗口探测最大间隔（ms）
IKCP_FASTACK_LIMIT = 5     // 快速重传次数上限
```

---

## 参考资料

- [KCP GitHub 仓库](https://github.com/skywind3000/kcp)
- [KCP Wiki - 最佳实践](https://github.com/skywind3000/kcp/wiki/KCP-Best-Practice)
- [KCP Wiki - 性能评测](https://github.com/skywind3000/kcp/wiki/KCP-Benchmark)
- [SpatialOS KCP 评测](https://improbable.io/blog/kcp-a-new-low-latency-secure-network-stack)
- [《原神》使用 KCP 加速游戏消息](https://skywind.me/blog/archives/2706)
